#include "widcc_jit.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define CK(jit, name, src) do { \
  if (widcc_jit_compile(jit, name, src) != 0) { \
    fprintf(stderr, "compile %s failed: %s\n", name, widcc_jit_error(jit)); \
    exit(1); \
  } \
} while (0)

static int add3(int a, int b, int c) { return a + b + c; }
static double dadd(double a, double b) { return a + b; }

int main(void) {
  WidccJIT *jit = widcc_jit_new();
  widcc_jit_map_symbol(jit, "add3", (void *)add3);
  widcc_jit_map_symbol(jit, "dadd", (void *)dadd);

  // Deep integer nesting: forces many simultaneous live values -> spills.
  CK(jit, "deep.c",
    "int deep(int a,int b,int c,int d){\n"
    "  return (a+b)*(c+d) + (a-b)*(c-d) + (a*c)-(b*d) + (a+d)*(b+c);\n"
    "}\n"
    "int deep2(int x){\n"
    "  return ((((x+1)*(x+2))+((x+3)*(x+4)))*(((x+5)*(x+6))+((x+7)*(x+8))));\n"
    "}\n");
  int (*deep)(int,int,int,int) = widcc_jit_symbol(jit, "deep");
  int (*deep2)(int) = widcc_jit_symbol(jit, "deep2");
  for (int a=-3;a<=3;a++) for(int b=-2;b<=2;b++){
    int c=a+1,d=b-1;
    int expect=(a+b)*(c+d)+(a-b)*(c-d)+(a*c)-(b*d)+(a+d)*(b+c);
    assert(deep(a,b,c,d)==expect);
  }
  for(int x=-4;x<=4;x++){
    int e=((((x+1)*(x+2))+((x+3)*(x+4)))*(((x+5)*(x+6))+((x+7)*(x+8))));
    assert(deep2(x)==e);
  }

  // Call with many live values across it (spill_all + lazy reload).
  CK(jit, "call.c",
    "int add3(int,int,int);\n"
    "double dadd(double,double);\n"
    "int callmid(int a,int b,int c,int d){\n"
    "  return (a+b) + add3(a,b,c) + (c*d) + add3(b,c,d) + (a-d);\n"
    "}\n"
    "double dcall(double a,double b,double c){\n"
    "  return (a*b) + dadd(a,b) + (b*c) + dadd(b,c) + (a-c);\n"
    "}\n");
  int (*callmid)(int,int,int,int) = widcc_jit_symbol(jit, "callmid");
  double (*dcall)(double,double,double) = widcc_jit_symbol(jit, "dcall");
  for(int a=1;a<=3;a++)for(int b=1;b<=3;b++){
    int c=a+b,d=b+1;
    int expect=(a+b)+add3(a,b,c)+(c*d)+add3(b,c,d)+(a-d);
    assert(callmid(a,b,c,d)==expect);
  }
  for(int a=1;a<=3;a++){
    double x=a*0.5,y=a+0.25,z=a-0.75;
    double expect=(x*y)+dadd(x,y)+(y*z)+dadd(y,z)+(x-z);
    double got=dcall(x,y,z);
    assert(got==expect);
  }

  // Mixed float/int comparisons and casts, immediate folding.
  CK(jit, "mix.c",
    "int mix(int x,double d){\n"
    "  int r=0;\n"
    "  if (d>0.5) r+=1;\n"
    "  if (x==3) r+=2;\n"
    "  if (x!=3) r+=4;\n"
    "  r += (int)d;\n"
    "  r += (x>2)?10:20;\n"
    "  return r;\n"
    "}\n"
    "double fbin(double a,double b){ return (a+b)*(a-b)/(a+1.0); }\n");
  int (*mix)(int,double) = widcc_jit_symbol(jit, "mix");
  double (*fbin)(double,double) = widcc_jit_symbol(jit, "fbin");
  assert(mix(3,1.5)==(1+2+1+10));
  assert(mix(2,0.25)==(0+0+4+0+20));
  for(int a=2;a<=5;a++){
    double x=a,y=a-1;
    double expect=(x+y)*(x-y)/(x+1.0);
    assert(fbin(x,y)==expect);
  }

  // Struct assignment with live values around it.
  CK(jit, "struct.c",
    "struct P{int x;int y;int z;};\n"
    "int st(int a,int b){\n"
    "  struct P p,q;\n"
    "  p.x=a;p.y=b;p.z=a+b;\n"
    "  q=p;\n"
    "  return (q.x+q.y+q.z) + (a*b);\n"
    "}\n");
  int (*st)(int,int) = widcc_jit_symbol(jit, "st");
  for(int a=1;a<=4;a++)for(int b=1;b<=4;b++)
    assert(st(a,b)==(a+b+(a+b))+(a*b));

  // Logical short-circuit with side effects and live values.
  CK(jit, "logic.c",
    "int g;\n"
    "int bump(void){ g++; return 1; }\n"
    "int lg(int a,int b){\n"
    "  g=0;\n"
    "  int r = (a>0 && bump()) + (b>0 || bump());\n"
    "  return r*100+g;\n"
    "}\n");
  int (*lg)(int,int) = widcc_jit_symbol(jit, "lg");
  assert(lg(1,1)==2*100+1);
  assert(lg(0,0)==1*100+1);
  assert(lg(1,0)==2*100+2);
  assert(lg(0,1)==1*100+0);

  // Loop with a call and spilled accumulators.
  CK(jit, "loop.c",
    "int add3(int,int,int);\n"
    "int lp(int n){\n"
    "  int s=0,p=1;\n"
    "  for(int i=0;i<n;i++){ s+=add3(i,i+1,i+2); p*=2; }\n"
    "  return s+p;\n"
    "}\n");
  int (*lp)(int) = widcc_jit_symbol(jit, "lp");
  for(int n=0;n<=6;n++){
    int s=0,p=1;
    for(int i=0;i<n;i++){s+=add3(i,i+1,i+2);p*=2;}
    assert(lp(n)==s+p);
  }

  // Huffman coding: build the tree, derive codes, encode a message,
  // decode it back, and check the weighted path length (CLRS frequencies
  // {45,13,12,16,9,5} have optimal WPL 224).
  CK(jit, "huffman.c",
    "int huffman(void){\n"
    "  int freq[6];\n"
    "  freq[0]=45; freq[1]=13; freq[2]=12; freq[3]=16; freq[4]=9; freq[5]=5;\n"
    "  int n=6;\n"
    "  int weight[12], left[12], right[12], parent[12];\n"
    "  for (int i=0;i<12;i++){\n"
    "    weight[i]=0; left[i]=-1; right[i]=-1; parent[i]=-1;\n"
    "  }\n"
    "  for (int i=0;i<n;i++) weight[i]=freq[i];\n"
    "  int total=n;\n"
    "  for (int k=0;k<n-1;k++){\n"
    "    int a=-1,b=-1;\n"
    "    for (int i=0;i<total;i++){\n"
    "      if (parent[i]!=-1) continue;\n"
    "      if (a==-1 || weight[i]<weight[a]) { b=a; a=i; }\n"
    "      else if (b==-1 || weight[i]<weight[b]) b=i;\n"
    "    }\n"
    "    weight[total]=weight[a]+weight[b];\n"
    "    left[total]=a; right[total]=b;\n"
    "    parent[a]=total; parent[b]=total;\n"
    "    total++;\n"
    "  }\n"
    "  int root=total-1;\n"
    "  int code[6], len[6];\n"
    "  for (int i=0;i<n;i++){\n"
    "    int c=0,l=0,node=i;\n"
    "    while (parent[node]!=-1){\n"
    "      int p=parent[node];\n"
    "      if (right[p]==node) c |= 1<<l;\n"
    "      l++; node=p;\n"
    "    }\n"
    "    code[i]=c; len[i]=l;\n"
    "  }\n"
    "  int wpl=0;\n"
    "  for (int i=0;i<n;i++) wpl+=freq[i]*len[i];\n"
    "  if (wpl!=224) return 1;\n"
    "  int msg[20];\n"
    "  int m=0;\n"
    "  for (int r=0;r<3;r++) for (int s=0;s<6;s++) msg[m++]=s;\n"
    "  msg[18]=5; msg[19]=4;\n"
    "  int bits[512]; int nb=0;\n"
    "  for (int i=0;i<20;i++){\n"
    "    int s=msg[i];\n"
    "    for (int j=len[s]-1;j>=0;j--) bits[nb++]=(code[s]>>j)&1;\n"
    "  }\n"
    "  int node=root, nd=0;\n"
    "  for (int i=0;i<nb;i++){\n"
    "    node = bits[i] ? right[node] : left[node];\n"
    "    if (left[node]==-1){\n"
    "      if (node!=msg[nd]) return 2;\n"
    "      nd++; node=root;\n"
    "    }\n"
    "  }\n"
    "  if (nd!=20 || node!=root) return 3;\n"
    "  return 0;\n"
    "}\n");
  int (*huffman)(void) = widcc_jit_symbol(jit, "huffman");
  assert(huffman() == 0);

  // Recursive Hoare quicksort + lower-bound binary search.
  CK(jit, "sort.c",
    "void qs(int *a,int lo,int hi){\n"
    "  if (hi-lo<2) return;\n"
    "  int mid=lo+(hi-lo)/2;\n"
    "  int t0=a[lo]; a[lo]=a[mid]; a[mid]=t0;\n"
    "  int p=a[lo];\n"
    "  int i=lo-1,j=hi;\n"
    "  for(;;){\n"
    "    do i++; while (a[i]<p);\n"
    "    do j--; while (a[j]>p);\n"
    "    if (i>=j) break;\n"
    "    int t=a[i]; a[i]=a[j]; a[j]=t;\n"
    "  }\n"
    "  qs(a,lo,j+1); qs(a,j+1,hi);\n"
    "}\n"
    "int lb(int *a,int n,int x){\n"
    "  int lo=0,hi=n;\n"
    "  while (lo<hi){\n"
    "    int m=(lo+hi)/2;\n"
    "    if (a[m]<x) lo=m+1; else hi=m;\n"
    "  }\n"
    "  if (lo<n && a[lo]==x) return lo;\n"
    "  return -1;\n"
    "}\n");
  void (*qs)(int *, int, int) = widcc_jit_symbol(jit, "qs");
  int (*lb)(int *, int, int) = widcc_jit_symbol(jit, "lb");
  {
    int arr[200];
    unsigned seed = 12345;
    long sum0 = 0;
    for (int i = 0; i < 200; i++) {
      seed = seed * 1103515245u + 12345u;
      arr[i] = (int)((seed >> 16) % 1000);
      sum0 += arr[i];
    }
    qs(arr, 0, 200);
    long sum1 = 0;
    for (int i = 0; i < 200; i++) {
      if (i > 0) assert(arr[i - 1] <= arr[i]);
      sum1 += arr[i];
    }
    assert(sum0 == sum1);
    for (int i = 0; i < 200; i += 17) {
      int idx = lb(arr, 200, arr[i]);
      assert(idx >= 0 && arr[idx] == arr[i]);
      assert(idx == 0 || arr[idx - 1] < arr[i]);
    }
    assert(lb(arr, 200, 1000) == -1);
  }

  // Sieve of Eratosthenes: pi(1000) = 168.
  CK(jit, "sieve.c",
    "int primes(int n){\n"
    "  int flag[1000];\n"
    "  for (int i=0;i<n;i++) flag[i]=1;\n"
    "  flag[0]=0; flag[1]=0;\n"
    "  for (int i=2;i*i<n;i++)\n"
    "    if (flag[i])\n"
    "      for (int j=i*i;j<n;j+=i) flag[j]=0;\n"
    "  int c=0;\n"
    "  for (int i=0;i<n;i++) c+=flag[i];\n"
    "  return c;\n"
    "}\n");
  int (*primes)(int) = widcc_jit_symbol(jit, "primes");
  assert(primes(1000) == 168);
  assert(primes(100) == 25);
  assert(primes(2) == 0);

  // 8x8 integer matrix product through flat pointers.
  CK(jit, "matmul.c",
    "void mmul(int *a,int *b,int *c,int n){\n"
    "  for (int i=0;i<n;i++)\n"
    "    for (int j=0;j<n;j++){\n"
    "      int s=0;\n"
    "      for (int k=0;k<n;k++) s+=a[i*n+k]*b[k*n+j];\n"
    "      c[i*n+j]=s;\n"
    "    }\n"
    "}\n");
  void (*mmul)(int *, int *, int *, int) = widcc_jit_symbol(jit, "mmul");
  {
    int a[64], b[64], c[64];
    for (int i = 0; i < 64; i++) { a[i] = i % 7 - 3; b[i] = (i * 3) % 5 - 2; }
    mmul(a, b, c, 8);
    for (int i = 0; i < 8; i++)
      for (int j = 0; j < 8; j++) {
        int s = 0;
        for (int k = 0; k < 8; k++) s += a[i * 8 + k] * b[k * 8 + j];
        assert(c[i * 8 + j] == s);
      }
  }

  // Sorted linked list over a static node pool, then in-place reversal.
  CK(jit, "list.c",
    "struct Node { int val; struct Node *next; };\n"
    "struct Node pool[64];\n"
    "int used;\n"
    "struct Node *alloc_node(int v){\n"
    "  struct Node *p=&pool[used]; used++;\n"
    "  p->val=v; p->next=0;\n"
    "  return p;\n"
    "}\n"
    "struct Node *insert(struct Node *head,int v){\n"
    "  struct Node *n=alloc_node(v);\n"
    "  if (!head || v<head->val){ n->next=head; return n; }\n"
    "  struct Node *cur=head;\n"
    "  while (cur->next && cur->next->val<v) cur=cur->next;\n"
    "  n->next=cur->next; cur->next=n;\n"
    "  return head;\n"
    "}\n"
    "struct Node *reverse(struct Node *head){\n"
    "  struct Node *prev=0;\n"
    "  while (head){\n"
    "    struct Node *nx=head->next;\n"
    "    head->next=prev; prev=head; head=nx;\n"
    "  }\n"
    "  return prev;\n"
    "}\n"
    "int list_check(void){\n"
    "  used=0;\n"
    "  struct Node *h=0;\n"
    "  h=insert(h,5); h=insert(h,3); h=insert(h,8); h=insert(h,1);\n"
    "  h=insert(h,9); h=insert(h,2); h=insert(h,7); h=insert(h,3);\n"
    "  h=reverse(h);\n"
    "  int expect[8];\n"
    "  expect[0]=9; expect[1]=8; expect[2]=7; expect[3]=5;\n"
    "  expect[4]=3; expect[5]=3; expect[6]=2; expect[7]=1;\n"
    "  int i=0;\n"
    "  while (h){\n"
    "    if (i>=8 || h->val!=expect[i]) return 1;\n"
    "    i++; h=h->next;\n"
    "  }\n"
    "  if (i!=8 || used!=8) return 2;\n"
    "  return 0;\n"
    "}\n");
  int (*list_check)(void) = widcc_jit_symbol(jit, "list_check");
  assert(list_check() == 0);

  // Newton-Raphson square root: float division/multiplication in a loop.
  CK(jit, "sqrt.c",
    "double dsqrt(double x){\n"
    "  if (x<=0.0) return 0.0;\n"
    "  double r=x;\n"
    "  for (int i=0;i<40;i++) r=(r+x/r)*0.5;\n"
    "  return r;\n"
    "}\n");
  double (*dsqrt)(double) = widcc_jit_symbol(jit, "dsqrt");
  {
    double xs[] = {2.0, 0.25, 100.0, 1e-6};
    for (int i = 0; i < 4; i++) {
      double r = dsqrt(xs[i]);
      double err = r * r - xs[i];
      if (err < 0) err = -err;
      assert(err <= 1e-9 * xs[i]);
    }
    assert(dsqrt(0.0) == 0.0);
  }

  widcc_jit_free(jit);
  printf("stress: all tests passed\n");
  return 0;
}