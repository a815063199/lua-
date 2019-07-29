/*
** $Id: ltable.c,v 2.32.1.2 2007/12/28 15:32:23 roberto Exp $
** Lua tables (hash)
** See Copyright Notice in lua.h
*/


/*
** Implementation of tables (aka arrays, objects, or hash tables).
** Tables keep its elements in two parts: an array part and a hash part.
** Non-negative integer keys are all candidates to be kept in the array
** part. The actual size of the array is the largest `n' such that at
** least half the slots between 0 and n are in use.
** Hash uses a mix of chained scatter table with Brent's variation.
** A main invariant of these tables is that, if an element is not
** in its main position (i.e. the `original' position that its hash gives
** to it), then the colliding element is in its own main position.
** Hence even when the load factor reaches 100%, performance remains good.
*/

#include <math.h>
#include <string.h>

#define ltable_c
#define LUA_CORE

#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "ltable.h"


/*
** max size of array part is 2^MAXBITS
*/
#if LUAI_BITSINT > 26
#define MAXBITS		26
#else
#define MAXBITS		(LUAI_BITSINT-2)
#endif

#define MAXASIZE	(1 << MAXBITS)

//sizenode(t)为散列表的长度
#define hashpow2(t,n)      (gnode(t, lmod((n), sizenode(t))))
  
#define hashstr(t,str)  hashpow2(t, (str)->tsv.hash)
#define hashboolean(t,p)        hashpow2(t, p)


/*
** for some types, it is better to avoid modulus by power of 2, as
** they tend to have many 2 factors.
*/
#define hashmod(t,n)	(gnode(t, ((n) % ((sizenode(t)-1)|1))))
/*
** #define twoto(x) (1<<(x)) (2的x次方)
** #define sizenode(t)  (twoto((t)->lsizenode))
** lsizenode是散列表长度的以2为底的对数值，sizenode(t)的值为实际散列表的长度(2^t->lsizenode)
*/

#define hashpointer(t,p)	hashmod(t, IntPoint(p))


/*
** number of ints inside a lua_Number
*/
#define numints		cast_int(sizeof(lua_Number)/sizeof(int))



#define dummynode		(&dummynode_)

static const Node dummynode_ = {
  {{NULL}, LUA_TNIL},  /* value */
  {{{NULL}, LUA_TNIL, NULL}}  /* key */
};


/*
** hash for lua_Numbers
** n = 0x0000 0001 0000 0002
** a[0] = 0x0000 0001, a[1] = 0x0000 0002
** a[0] = a[0] + a[1] = 3
** hashmod(t, a[0])
** gnode(t, a[0] % ( (散列表长度 - 1)|1 ))
** gnode(t, 3    % ( (8 -1)|1 )
** gnode(t, 3)
** 该函数的作用是，为所有类型为lua_Numbers的关键字，在散列表部分确定索引位置。
*/
static Node *hashnum (const Table *t, lua_Number n) {
  unsigned int a[numints];
  int i;
  if (luai_numeq(n, 0))  /* avoid problems with -0 */
    return gnode(t, 0);
  memcpy(a, &n, sizeof(a));
  for (i = 1; i < numints; i++) a[0] += a[i];
  return hashmod(t, a[0]);
/*
**  #define gnode(t,i) (&(t)->node[i])
*/
}



/*
** returns the `main' position of an element in a table (that is, the index
** of its hash value)
** mainposition是指table散列表部分的索引
** 根据lua_Number，str或者是其他类型的值来进行hash，所得到的hash值可能相等，
** 相等的hash值在散列表部分共享同一个索引项（mainposition），也就是说，散列表索引项所指向的链表
** 中，值的类型既可能是lua_Number，也可能是str或者其他。
**     ______
**    | Node |-->[ lua_Number n ]
**    |______|
**    | Node |-->[ int b ]-->[ GCObject *gc ]
**    |______|
**    | Node |-->[void* p]
**    |______|
**    | Node |
**     ------

*/
static Node *mainposition (const Table *t, const TValue *key) {
  switch (ttype(key)) {
    case LUA_TNUMBER:
      return hashnum(t, nvalue(key));
    case LUA_TSTRING:
      return hashstr(t, rawtsvalue(key));
    case LUA_TBOOLEAN:
      return hashboolean(t, bvalue(key));
    case LUA_TLIGHTUSERDATA:
      return hashpointer(t, pvalue(key));
    default:
      return hashpointer(t, gcvalue(key));
  }
/*
**  #define nvalue(o) check_exp(ttisnumber(o), (o)->value.n)
**  #define rawtsvalue(o) check_exp(ttisstring(o), &(o)->value.gc->ts)
**  #define bvalue(o) check_exp(ttisboolean(o), (o)->value.b)
**  #define pvalue(o) check_exp(ttislightuserdata(o), (o)->value.p)
**  #define gcvalue(o) check_exp(iscollectable(o), (o)->value.gc)
*/
}


/*
** returns the index for `key' if `key' is an appropriate key to live in
** the array part of the table, -1 otherwise.
** #define nvalue(o)  check_exp(ttisnumber(o), (o)->value.n)
** #define lua_number2int(i,d)  ((i)=(int)(d))
** 如果key是lua_Number类型，且其值为2.5,那么它不能放在表t的数组部分
** 如果key是lua_Number类型，且其值为2,那么它在数组的索引位置为2
*/
static int arrayindex (const TValue *key) {
  if (ttisnumber(key)) {
    lua_Number n = nvalue(key);
    int k;
    lua_number2int(k, n);
    if (luai_numeq(cast_num(k), n))
      return k;
  }
  return -1;  /* `key' did not match some condition */
}


/*
** returns the index of a `key' for table traversals. First goes all
** elements in the array part, then elements in the hash part. The
** beginning of a traversal is signalled by -1.
*/
static int findindex (lua_State *L, Table *t, StkId key) {
  int i;
  if (ttisnil(key)) return -1;  /* first iteration */
  i = arrayindex(key);
  if (0 < i && i <= t->sizearray)  /* is `key' inside array part? */
    return i-1;  /* yes; that's the index (corrected to C) */
  else {
    Node *n = mainposition(t, key);
    do {  /* check whether `key' is somewhere in the chain */
      /* key may be dead already, but it is ok to use it in `next' */
      if (luaO_rawequalObj(key2tval(n), key) ||
            (ttype(gkey(n)) == LUA_TDEADKEY && iscollectable(key) &&
             gcvalue(gkey(n)) == gcvalue(key))) {
        i = cast_int(n - gnode(t, 0));  /* key index in hash table */
        /* hash elements are numbered after array ones */
        return i + t->sizearray;
      }
      else n = gnext(n);
    } while (n);
    luaG_runerror(L, "invalid key to " LUA_QL("next"));  /* key not found */
    return 0;  /* to avoid warnings */
  }
}


int luaH_next (lua_State *L, Table *t, StkId key) {
  int i = findindex(L, t, key);  /* find original element */
  for (i++; i < t->sizearray; i++) {  /* try first array part */
    if (!ttisnil(&t->array[i])) {  /* a non-nil value? */
      setnvalue(key, cast_num(i+1));
      setobj2s(L, key+1, &t->array[i]);
      return 1;
    }
  }
  for (i -= t->sizearray; i < sizenode(t); i++) {  /* then hash part */
    if (!ttisnil(gval(gnode(t, i)))) {  /* a non-nil value? */
      setobj2s(L, key, key2tval(gnode(t, i)));
      setobj2s(L, key+1, gval(gnode(t, i)));
      return 1;
    }
  }
  return 0;  /* no more elements */
}


/*
** {=============================================================
** Rehash
** ==============================================================
*/

/*
** na = 准备进入数组部分的节点个数
**  n = 最优的数组部分大小, 为2的次幂
** 该函数保证: 数组部分使用的节点个数na要大于等于数组总大小的一半, 例如：
**  nums[0] = 1
**  nums[1] = 1
**  nums[2] = 2
**  nums[3] = 4
**  nums[4] = 3
**  nums[5] = 0
**  nums[6] = 0
**  nums[7] = 0
**  nums[8] = 1
** nums是原表t的数组部分节点使用情况, nums[i]表示在array索引位置2^(i-1)至2^i - 1之间，被使用的节点个数
** 通过该函数计算后, 得到的na = 11, n = 16
*/
static int computesizes (int nums[], int *narray) {
  int i;
  int twotoi;  /* 2^i */
  int a = 0;  /* number of elements smaller than 2^i */
  int na = 0;  /* number of elements to go to array part */
  int n = 0;  /* optimal size for array part */
  for (i = 0, twotoi = 1; twotoi/2 < *narray; i++, twotoi *= 2) {
    if (nums[i] > 0) {
      a += nums[i];
      if (a > twotoi/2) {  /* more than half elements present? */
        n = twotoi;  /* optimal size (till now) */
        na = a;  /* all elements smaller than n will go to array part */
      }
    }
    if (a == *narray) break;  /* all elements already counted */
  }
  *narray = n;
  lua_assert(*narray/2 <= na && na <= *narray);
  return na;
}

/*
** //ceil理解为天花板，该宏求的是"log以2为底x的对数向上取整"
** #define ceillog2(x) (luaO_log2((x)-1) + 1)
*/
static int countint (const TValue *key, int *nums) {
  int k = arrayindex(key); /* 如果 k = 250 */
  if (0 < k && k <= MAXASIZE) {  /* is `key' an appropriate array index? */
    nums[ceillog2(k)]++;  /* count as such */
    return 1;
  }
  else
    return 0;
}

/*
** 该函数计算t的数组部分使用的节点个数，例如：
**   0 -> [ T ]
**   1 -> [ T ]
**   2 -> [ T ]
**   3 -> [ T ]
**   4 -> [ T ]
**   5 -> [ T ]
**   6 -> [ T ]
**   7 -> [ T ]
**   8 -> [ T ]
**   9 -> [ T ]
**  10 -> [ T ]
**  11 -> [ 0 ]
**  12 -> [ 0 ]
**  13 -> [ 0 ]
**  14 -> [ 0 ]
**  15 -> [ 0 ]
** 已上表示表t的数组array部分，array[0..9]11个元素是在使用的，array[11..15]5个元素没有被使用。
** numusearray函数调用后，ause = 11， nums数组的值如下：
**   0 -> [ 1 ]
**   1 -> [ 1 ]
**   2 -> [ 2 ]
**   3 -> [ 4 ]
**   4 -> [ 3 ]
**  可以看出nums数组的和等于ause，nums[i]表示在array索引位置2^(i-1)至2^i - 1之间，被使用的节点个数。
*/
static int numusearray (const Table *t, int *nums) {
  int lg;
  int ttlg;  /* 2^lg */
  int ause = 0;  /* summation of `nums' */
  int i = 1;  /* count to traverse all array keys */
  for (lg=0, ttlg=1; lg<=MAXBITS; lg++, ttlg*=2) {  /* for each slice */
    int lc = 0;  /* counter */
    int lim = ttlg;
    if (lim > t->sizearray) {
      lim = t->sizearray;  /* adjust upper limit */
      if (i > lim)
        break;  /* no more elements to count */
    }
    /* count elements in range (2^(lg-1), 2^lg] */
    for (; i <= lim; i++) {
      if (!ttisnil(&t->array[i-1]))
        lc++;
    }
    nums[lg] += lc;
    ause += lc;
  }
  return ause;
}

/*
** 该函数计算表t散列表使用的节点个数
** #define gval(n)    (&(n)->i_val)
** #define key2tval(n)  (&(n)->i_key.tvk)
*/
static int numusehash (const Table *t, int *nums, int *pnasize) {
  int totaluse = 0;  /* total number of elements */
  int ause = 0;  /* summation of `nums' */
  int i = sizenode(t);
  while (i--) {
    Node *n = &t->node[i];
    if (!ttisnil(gval(n))) {
      ause += countint(key2tval(n), nums);
      totaluse++;
    }
  }
  *pnasize += ause;
  return totaluse;
}

/*
** #define luaM_reallocvector(L, v,oldn,n,t) \
**   ((v)=cast(t *, luaM_reallocv(L, v, oldn, n, sizeof(t))))
*/
static void setarrayvector (lua_State *L, Table *t, int size /* new array size */) {
  int i;
  luaM_reallocvector(L, t->array, t->sizearray, size, TValue);
  for (i=t->sizearray; i<size; i++)
     setnilvalue(&t->array[i]);
  t->sizearray = size;
}

/*
** #define twoto(x) (1<<(x))
** #define gnode(t,i) (&(t)->node[i])
** #define gkey(n)    (&(n)->i_key.nk)
** #define gval(n)   (&(n)->i_val)
*/
static void setnodevector (lua_State *L, Table *t, int size /* new hash size */) {
  int lsize;
  if (size == 0) {  /* no elements to hash part? */
    t->node = cast(Node *, dummynode);  /* use common `dummynode' */
    lsize = 0;
  }
  else {
    int i;
    lsize = ceillog2(size);
    if (lsize > MAXBITS)
      luaG_runerror(L, "table overflow");
    size = twoto(lsize);
    t->node = luaM_newvector(L, size, Node);
    for (i=0; i<size; i++) {
      Node *n = gnode(t, i);
      gnext(n) = NULL;
      setnilvalue(gkey(n));
      setnilvalue(gval(n));
    }
  }
  t->lsizenode = cast_byte(lsize);
  t->lastfree = gnode(t, size);  /* all positions are free */
}

/*
** #define setobjt2t  setobj
** #define setobj(L,obj1,obj2) \
**  { const TValue *o2=(obj2); TValue *o1=(obj1); \
**    o1->value = o2->value; o1->tt=o2->tt; \
**    checkliveness(G(L),o1); }
*/
static void resize (lua_State *L, Table *t, int nasize, int nhsize) {
  int i;
  int oldasize = t->sizearray;
  int oldhsize = t->lsizenode;
  Node *nold = t->node;  /* save old hash ... */
  if (nasize > oldasize)  /* array part must grow? */
    setarrayvector(L, t, nasize); //新的数组部分包括原来的数据，加上新分配的空间
  /* create new hash part with appropriate size
  ** 为新的散列表部分分配空间，新的散列表部分不包括原来的数据，全部置为空
  */
  setnodevector(L, t, nhsize); 
  /* 如果新数组的大小小于旧数组的大小 */
  if (nasize < oldasize) {  /* array part must shrink? */
    t->sizearray = nasize;
    /* re-insert elements from vanishing slice
    ** 将原数组部分的节点（超出新数组大小范围内的节点）重新插入到新的散列表部分
    */
    for (i=nasize; i<oldasize; i++) {
      if (!ttisnil(&t->array[i]))
        /* setobjt2t(L, i+1作为key插入到新的散列表节点node对应的值TValue, 原数组索引位置i节点(对应的关键字key当然是i+1)TValue) */
        setobjt2t(L, luaH_setnum(L, t, i+1), &t->array[i]);
    }
    /* shrink array */
    luaM_reallocvector(L, t->array, oldasize, nasize, TValue);
  }
  /* re-insert elements from hash part
  ** 将旧的散列表部分的节点从最后一个开始逐个插入到新的散列表中
  */
  for (i = twoto(oldhsize) - 1; i >= 0; i--) {
    Node *old = nold+i;
    if (!ttisnil(gval(old)))
      setobjt2t(L, luaH_set(L, t, key2tval(old)), gval(old));
  }
  /* 释放掉原散列表的空间 */
  if (nold != dummynode)
    luaM_freearray(L, nold, twoto(oldhsize), Node);  /* free old array */
}


void luaH_resizearray (lua_State *L, Table *t, int nasize) {
  int nsize = (t->node == dummynode) ? 0 : sizenode(t);
  resize(L, t, nasize, nsize);
}

/*
** nasize = 表t数组部分使用的总结点数, 再加上要加入表t的new key: ek (如果ek是可以放在数组部分的值)
** totaluse = 表t数组部分和散列表部分使用的总结点数, 再加上要加入表t的new key: ek
** na = 新表的数组部分使用的节点总数
** totaluse - na = 新表的散列表部分的大小
*/
static void rehash (lua_State *L, Table *t, const TValue *ek) {
  int nasize, na;
  int nums[MAXBITS+1];  /* nums[i] = number of keys between 2^(i-1) and 2^i */
  int i;
  int totaluse;
  for (i=0; i<=MAXBITS; i++) nums[i] = 0;  /* reset counts */
  nasize = numusearray(t, nums);  /* count keys in array part */
  totaluse = nasize;  /* all those keys are integer keys 数组部分的key全是整型的*/
  totaluse += numusehash(t, nums, &nasize);  /* count keys in hash part */
  /* count extra key */
  nasize += countint(ek, nums);
  totaluse++;
  /* compute new size for array part */
  na = computesizes(nums, &nasize);
  /* resize the table to new computed sizes */
  resize(L, t, nasize, totaluse - na);
}



/*
** }=============================================================
*/


Table *luaH_new (lua_State *L, int narray, int nhash) {
  Table *t = luaM_new(L, Table);
  luaC_link(L, obj2gco(t), LUA_TTABLE);
  t->metatable = NULL;
  t->flags = cast_byte(~0);
  /* temporary values (kept only if some malloc fails) */
  t->array = NULL;
  t->sizearray = 0;
  t->lsizenode = 0;
  t->node = cast(Node *, dummynode);
  setarrayvector(L, t, narray);
  setnodevector(L, t, nhash);
  return t;
}


void luaH_free (lua_State *L, Table *t) {
  if (t->node != dummynode)
    luaM_freearray(L, t->node, sizenode(t), Node);
  luaM_freearray(L, t->array, t->sizearray, TValue);
  luaM_free(L, t);
}

/*
** 注意此函数会导致表t的lastfree指针前移，这说明了，在表t中新增关键字Node时，
** 会直接拿散列数组末尾的节点，不会重新分配一个节点空间。
*/
static Node *getfreepos (Table *t) {
  while (t->lastfree-- > t->node) {
    if (ttisnil(gkey(t->lastfree)))
      return t->lastfree;
  }
  return NULL;  /* could not find a free place */
}



/*
** inserts a new key into a hash table; first, check whether key's main 
** position is free. If not, check whether colliding node is in its main 
** position or not: if it is not, move colliding node to an empty place and 
** put new key in its main position; otherwise (colliding node is in its main 
** position), new key goes to an empty position. 
**
** #define key2tval(n)  (&(n)->i_key.tvk)
** #define gkey(n)    (&(n)->i_key.nk)
** #define gval(n)    (&(n)->i_val)
*/
static TValue *newkey (lua_State *L, Table *t, const TValue *key) {
  Node *mp = mainposition(t, key);
  //判断在该mainposition上是否已经有值
  if (!ttisnil(gval(mp)) || mp == dummynode) {
    Node *othern;
    Node *n = getfreepos(t);  /* 从散列表部分的最后一个节点开始查找，直到找出一个空位置*/
    if (n == NULL) {  /* cannot find a free place? */
      rehash(L, t, key);  /* grow table */
      return luaH_set(L, t, key);  /* re-insert key into grown table */
    }
    lua_assert(n != dummynode);
    othern = mainposition(t, key2tval(mp));
    if (othern != mp) {  /* is colliding node out of its main position? */
      /* yes; move colliding node into free position */
      while (gnext(othern) != mp) othern = gnext(othern);  /* find previous */
      gnext(othern) = n;  /* redo the chain with `n' in place of `mp' */
      *n = *mp;  /* copy colliding node into free pos. (mp->next also goes 这个注释很重要) */
      gnext(mp) = NULL;  /* now `mp' is free */
      setnilvalue(gval(mp));
    }
    else {  /* colliding node is in its own main position */
      /* new node will go into free position */
      gnext(n) = gnext(mp);  /* chain new position */
      gnext(mp) = n;
      mp = n;
    }
  }
  gkey(mp)->value = key->value; gkey(mp)->tt = key->tt;
  luaC_barriert(L, t, key);
  lua_assert(ttisnil(gval(mp)));
  return gval(mp);
}


/*
** search function for integers
** 在表t中查找整型值key
*/
const TValue *luaH_getnum (Table *t, int key) {
  /* (1 <= key && key <= t->sizearray)
  ** 如果key值在1~sizearray之间，返回数组内的元素指针Tvalue*，否则在散列表中查找
  */
  if (cast(unsigned int, key-1) < cast(unsigned int, t->sizearray))
    return &t->array[key-1];
  else {
    /*
    **  首先利用hashnum函数找出该key值在散列表部分对应的索引，然后从该索引处开始查找：
    **  在散列表部分查看key为number且key值等于nk的node节点，
    **  找到后返回该节点key对应的value值的指针
    */
    lua_Number nk = cast_num(key);
    Node *n = hashnum(t, nk);
    do {  /* check whether `key' is somewhere in the chain */
      if (ttisnumber(gkey(n)) && luai_numeq(nvalue(gkey(n)), nk))
        return gval(n);  /* that's it */
      else n = gnext(n);
    } while (n);
    return luaO_nilobject;
  }
/*
**  #define gkey(n)    (&(n)->i_key.nk)
**  #define gval(n)    (&(n)->i_val)
**  #define gnext(n) ((n)->i_key.nk.next)
**  #define nvalue(o)  check_exp(ttisnumber(o), (o)->value.n)
**  #define rawtsvalue(o) check_exp(ttisstring(o), &(o)->value.gc->ts)
*/
}


/*
**  search function for strings
**  在表t中查找字符串类型key
*/
const TValue *luaH_getstr (Table *t, TString *key) {
  Node *n = hashstr(t, key); //通过key的hash值找出mainposition
  do {  /* check whether `key' is somewhere in the chain */
    if (ttisstring(gkey(n)) && rawtsvalue(gkey(n)) == key)
      return gval(n);  /* that's it */
    else n = gnext(n);
  } while (n);
  return luaO_nilobject;
}


/*
** main search function
** luaH_get(l_registry, "_LOADED")
*/
const TValue *luaH_get (Table *t, const TValue *key) {
  switch (ttype(key)) {
    case LUA_TNIL: return luaO_nilobject;
    case LUA_TSTRING: return luaH_getstr(t, rawtsvalue(key));
    case LUA_TNUMBER: {
      int k;
      lua_Number n = nvalue(key);
      lua_number2int(k, n);
      if (luai_numeq(cast_num(k), nvalue(key))) /* index is int? */
        return luaH_getnum(t, k);  /* use specialized version */
      /* else go through */
    }
    default: {
      Node *n = mainposition(t, key);
      do {  /* check whether `key' is somewhere in the chain */
        if (luaO_rawequalObj(key2tval(n), key))
          return gval(n);  /* that's it */
        else n = gnext(n);
      } while (n);
      return luaO_nilobject;
    }
  }
}


TValue *luaH_set (lua_State *L, Table *t, const TValue *key) {
  const TValue *p = luaH_get(t, key);
  t->flags = 0;
  if (p != luaO_nilobject)
    return cast(TValue *, p);
  else {
    if (ttisnil(key)) luaG_runerror(L, "table index is nil");
    else if (ttisnumber(key) && luai_numisnan(nvalue(key)))
      luaG_runerror(L, "table index is NaN");
    return newkey(L, t, key);
  }
}


TValue *luaH_setnum (lua_State *L, Table *t, int key) {
  const TValue *p = luaH_getnum(t, key);
  if (p != luaO_nilobject)
    return cast(TValue *, p);
  else {
    TValue k;
    setnvalue(&k, cast_num(key));
    return newkey(L, t, &k);
  }
}


TValue *luaH_setstr (lua_State *L, Table *t, TString *key) {
  const TValue *p = luaH_getstr(t, key);
  if (p != luaO_nilobject)
    return cast(TValue *, p);
  else {
    TValue k;
    setsvalue(L, &k, key);  //设置k的tt值以及value值
    return newkey(L, t, &k);//将k插入表t中
  }
}

/*
** 以在区间(i, j)调用luaH_getnum，如果在j处调用luaH_getnum(t, j)成功，
** 那么继续在区间(j, 2*j)之间调用luaH_getnum，如果luaH_getnum(t, 2*j)调用失败，
** 那么在(j, 2*j)之间进行二分查找，确定出边界
*/
static int unbound_search (Table *t, unsigned int j) {
  unsigned int i = j;  /* i is zero or a present index */
  j++;
  /* find `i' and `j' such that i is present and j is not */
  while (!ttisnil(luaH_getnum(t, j))) {
    i = j;
    j *= 2;
    if (j > cast(unsigned int, MAX_INT)) {  /* overflow? */
      /* table was built with bad purposes: resort to linear search */
      i = 1;
      while (!ttisnil(luaH_getnum(t, i))) i++;
      return i - 1;
    }
  }
  /* now do a binary search between them */
  while (j - i > 1) {
    unsigned int m = (i+j)/2;
    if (ttisnil(luaH_getnum(t, m))) j = m;
    else i = m;
  }
  return i;
}


/*
** Try to find a boundary in table `t'. A `boundary' is an integer index
** such that t[i] is non-nil and t[i+1] is nil (and 0 if t[1] is nil).
** 该函数在table中查找某一个边界key，边界是一个整型key值，这个key满足 t[key] != nil and t[key+1] == nil
** 先在数组部分找边界：判断数组最后一个元素是否是nil，如果不是，存在边界值，二分查找法在数组部分查找
** 数组部分不存在边界，那么在散列表部分查找
*/
int luaH_getn (Table *t) {
  unsigned int j = t->sizearray;
  if (j > 0 && ttisnil(&t->array[j - 1])) {
    /* there is a boundary in the array part: (binary) search for it */
    unsigned int i = 0;
    while (j - i > 1) {
      unsigned int m = (i+j)/2;
      if (ttisnil(&t->array[m - 1])) j = m;
      else i = m;
    }
    return i;
  }
  /* else must find a boundary in hash part */
  else if (t->node == dummynode)  /* 散列表部分为空，还没有值存在于散列表部分，直接返回数组的大小sizearray*/
    return j;  /* that is easy... */
  else return unbound_search(t, j);
}



#if defined(LUA_DEBUG)

Node *luaH_mainposition (const Table *t, const TValue *key) {
  return mainposition(t, key);
}

int luaH_isdummy (Node *n) { return n == dummynode; }

#endif
