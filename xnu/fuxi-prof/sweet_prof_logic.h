#ifndef SWEET_PROF_LOGIC_H
#define SWEET_PROF_LOGIC_H
#define SP_THREADS 256u
#define SP_BUCKETS 8192u
#define SP_PROBES 64u
#define SP_TOP 12u
#define SP_LINES 398u
struct sp_sample { uint64_t tid,pc,lr,bt[8],event; int x16,wr; unsigned state; };
struct sp_key { struct sp_sample s; unsigned count,mixed,owner; };
struct sp_thread { uint64_t tid; unsigned total,dropped; char name[64]; };
struct sp_table { struct sp_key keys[SP_BUCKETS]; struct sp_thread threads[SP_THREADS]; unsigned nt,untracked,sweeps,late,skipped; };
static unsigned sp_thread_index(struct sp_table *p,uint64_t tid) {
 for(unsigned i=0;i<p->nt;i++)if(p->threads[i].tid==tid)return i;
 if(p->nt==SP_THREADS){p->untracked++;return SP_THREADS;}
 unsigned i=p->nt++;p->threads[i].tid=tid;return i;
}
static uint64_t sp_hash(const struct sp_sample *s) {
 uint64_t h=s->tid^(s->state*0x9e3779b97f4a7c15ULL);
 const uint64_t v[]={s->pc,s->lr,s->bt[0],s->bt[1]};
 for(unsigned i=0;i<4;i++){h^=v[i];h^=h>>30;h*=0xbf58476d1ce4e5b9ULL;h^=h>>27;}
 return h^(h>>31);
}
static int sp_equal(const struct sp_sample *a,const struct sp_sample *b) {
 return a->tid==b->tid && a->state==b->state && a->pc==b->pc && a->lr==b->lr && a->bt[0]==b->bt[0] && a->bt[1]==b->bt[1];
}
static void sp_add(struct sp_table *p,unsigned owner,const struct sp_sample *s) {
 struct sp_thread *t=&p->threads[owner];t->total++;
 unsigned hash=(unsigned)sp_hash(s);
 for(unsigned i=0;i<SP_PROBES;i++) {
  struct sp_key *k=&p->keys[(hash+i)&(SP_BUCKETS-1)];
  if(!k->count){k->s=*s;k->owner=owner;k->count=1;return;}
  if(sp_equal(&k->s,s)) {
   k->count++;k->mixed|=k->s.x16!=s->x16 || k->s.event!=s->event || k->s.wr!=s->wr || k->s.bt[2]!=s->bt[2] || k->s.bt[3]!=s->bt[3];return;
  }
 }
 t->dropped++;
}
static void sp_name(char *name) {
 name[63]=0;
 for(unsigned i=0;name[i];i++)if((unsigned char)name[i]<=32 || (unsigned char)name[i]>126)name[i]='_';
 if(!name[0]){name[0]='-';name[1]=0;}
}
static void sp_print(struct sp_table *p) {
 static const char * const states[]={"run","wait","ready","stop"};
 unsigned omitted=0,quota=p->nt?(SP_LINES-p->nt)/p->nt:0,extra=p->nt?(SP_LINES-p->nt)%p->nt:0;
 for(unsigned t=0;t<p->nt;t++) {
  struct sp_key *top[SP_TOP]={0};struct sp_thread *th=&p->threads[t];
  for(unsigned i=0;i<SP_BUCKETS;i++) {
   struct sp_key *k=&p->keys[i];if(!k->count || k->owner!=t)continue;
   for(unsigned n=0;n<SP_TOP;n++)if(!top[n] || k->count>top[n]->count) {
    for(unsigned j=SP_TOP-1;j>n;j--)top[j]=top[j-1];top[n]=k;break;
   }
  }
  unsigned limit=quota+(t<extra);if(limit>SP_TOP)limit=SP_TOP;
  sp_name(th->name);
  printf("PROF backboardd t%llu name=%s total=%u dropped=%u\n",(unsigned long long)th->tid,th->name,th->total,th->dropped);
  for(unsigned n=0;n<SP_TOP && top[n];n++) {
   if(n==limit){omitted++;break;}
   struct sp_key *k=top[n];struct sp_sample *s=&k->s;
   printf("PROF backboardd t%llu name=%s n=%u/%u state=%s x16=%d wait=0x%llx wr=%d mixed=%u pc 0x%llx lr 0x%llx bt 0x%llx 0x%llx 0x%llx 0x%llx 0x%llx 0x%llx 0x%llx 0x%llx\n",
    (unsigned long long)th->tid,th->name,k->count,th->total,states[s->state],s->x16,(unsigned long long)s->event,s->wr,k->mixed,
    (unsigned long long)s->pc,(unsigned long long)s->lr,(unsigned long long)s->bt[0],(unsigned long long)s->bt[1],(unsigned long long)s->bt[2],(unsigned long long)s->bt[3],
    (unsigned long long)s->bt[4],(unsigned long long)s->bt[5],(unsigned long long)s->bt[6],(unsigned long long)s->bt[7]);
  }
 }
 printf("PROF done threads=%u sweeps=%u late=%u skipped=%u untracked=%u truncated=%u\n",p->nt,p->sweeps,p->late,p->skipped,p->untracked,omitted);
}
#endif
