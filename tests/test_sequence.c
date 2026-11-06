#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "sequence.h"
#include "config_json.h"

typedef struct { unsigned n; uint8_t key[256], mods[256]; bool reject; } sink_t;
static bool emit(void *p, uint8_t mods, uint8_t key) {
    sink_t *s=p;
    if(s->reject)return false;
    assert(s->n<256);s->key[s->n]=key;s->mods[s->n++]=mods;return true;
}
static void tick(engine_t *e,sink_t *s,uint32_t t) {engine_tick(e,t,true,true,emit,s);}
static void setup(engine_t *e,sink_t *s,sequence_t *c) {
    memset(s,0,sizeof(*s));engine_init(e,c);assert(!engine_start(e));tick(e,s,0);
    assert(!e->running && s->n==1 && s->key[0]==0);assert(engine_start(e));tick(e,s,1);s->n=0;
}
static void check_sequences(void) {
    engine_t e;sink_t s;sequence_t c;sequence_defaults(&c);c.loops=2;setup(&e,&s,&c);
    uint32_t press_at=0,release_at=0;unsigned presses=0;
    for(uint32_t now=2;now<4000;now++) {
        unsigned old=s.n;tick(&e,&s,now);
        if(s.n!=old){if(s.key[old]){assert(s.key[old]==30+presses%3);if(presses)assert(now-release_at>=500);press_at=now;presses++;}
          else {if(e.phase==PHASE_WAIT){assert(now-press_at>=40);release_at=now;}}}
    }
    assert(presses==6 && e.completed==2 && !e.running && !e.held);
    puts("PASS: sequence order, 40ms hold, >=500ms release gap, exactly two loops");
    setup(&e,&s,&c);tick(&e,&s,2);assert(e.held);
    engine_stop(&e);s.reject=true;tick(&e,&s,3);assert(e.held && e.release_pending);
    s.reject=false;tick(&e,&s,4);assert(!e.held && !e.release_pending && !e.running && s.key[s.n-1]==0);
    puts("PASS: stop during press retries release until accepted");
    setup(&e,&s,&c);s.reject=true;tick(&e,&s,2);assert(!e.held && e.index==0);s.reject=false;tick(&e,&s,3);assert(e.held && s.key[0]==30);
    engine_tick(&e,4,false,false,emit,&s);assert(!e.running);tick(&e,&s,5);assert(!e.running && s.key[s.n-1]==0);
    puts("PASS: failed press retry; USB reconnect stays paused and clears keys");
    c.actions[0]=(action_t){.is_wait=true,.wait_ms=100};c.count=1;c.loops=3;setup(&e,&s,&c);
    for(uint32_t t=2;t<500;t++)tick(&e,&s,t);
    assert(e.completed==3 && !e.running);for(unsigned i=0;i<s.n;i++)assert(!s.key[i]);
    puts("PASS: standalone waits and finite repetitions without key emission");
    c.actions[0]=(action_t){.key=6,.mods=9,.hold_ms=40,.wait_ms=0};c.loops=1;setup(&e,&s,&c);
    tick(&e,&s,UINT32_MAX-20);assert(s.key[0]==6 && s.mods[0]==9);
    tick(&e,&s,18);assert(e.held);tick(&e,&s,19);assert(!e.held);
    puts("PASS: modifier combination and millisecond counter wraparound");
    c.loops=0;setup(&e,&s,&c);for(uint32_t t=2;t<1000;t++)tick(&e,&s,t);assert(e.running && e.completed>10);engine_stop(&e);tick(&e,&s,1000);assert(!e.held);
    puts("PASS: unlimited looping can be stopped");
}
static void check_config(void) {
 sequence_t c,original;sequence_defaults(&original);c=original;char err[160];
 const char *bad[]={"{}","[]","null","{\"loops\":-1,\"actions\":[]}","{\"loops\":0.5,\"actions\":[]}","{\"loops\":0,\"actions\":[]}","{\"loops\":0,\"actions\":[{\"type\":\"wait\",\"wait\":0}]}","{\"loops\":0,\"actions\":[{\"type\":\"key\",\"key\":30,\"mods\":0,\"hold\":0,\"wait\":500}]}","{\"loops\":0,\"actions\":[{\"type\":\"wait\",\"wait\":86400001}]}","{\"loops\":0,\"actions\":[{\"type\":\"wait\",\"wait\":5}]}junk","{\"loops\":0,\"actions\":[{\"type\":\"bad\",\"wait\":5}]}","{\"loops\":0,\"actions\":[{\"type\":\"key\",\"key\":30,\"mods\":16,\"hold\":40,\"wait\":500}]}"};
 for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++){assert(!config_parse(bad[i],&c,err,sizeof(err)));assert(!memcmp(&c,&original,sizeof(c)));}
 char *encoded=config_encode(&original);assert(encoded);assert(config_parse(encoded,&c,err,sizeof(err)));char *roundtrip=config_encode(&c);assert(roundtrip && !strcmp(encoded,roundtrip));free(roundtrip);free(encoded);
 const char *valid="{\"loops\":100000,\"actions\":[{\"type\":\"wait\",\"wait\":86400000},{\"type\":\"key\",\"key\":115,\"mods\":15,\"hold\":10000,\"wait\":0}]}";
 assert(config_parse(valid,&c,err,sizeof(err)));assert(c.count==2 && c.actions[1].mods==15);
 c.count=32;for(unsigned i=0;i<32;i++)c.actions[i]=(action_t){.is_wait=true,.wait_ms=1};encoded=config_encode(&c);assert(config_parse(encoded,&original,err,sizeof(err)));free(encoded);
 puts("PASS: configuration round trip, 32 actions, bounds, malformed input, atomic rejection");
}
static void check_scheduler(void) {
 sequence_t c[4];for(unsigned i=0;i<4;i++){sequence_defaults(&c[i]);c[i].count=1;c[i].loops=1;c[i].actions[0]=(action_t){.key=30+i,.mods=i,.hold_ms=40,.wait_ms=100};}
 scheduler_t q;sink_t sink={0};scheduler_init(&q,c);
 scheduler_tick(&q,0,true,true,emit,&sink);
 for(unsigned i=0;i<4;i++)assert(engine_start(&q.engines[i]));
 unsigned pressed=0;int held=-1;uint32_t at=0;
 for(uint32_t t=1;t<500;t++){
  unsigned before=sink.n;scheduler_tick(&q,t,true,true,emit,&sink);assert(sink.n-before<=1);
  if(sink.n>before){if(sink.key[before]){assert(held==-1);assert(sink.key[before]==30+pressed);assert(sink.mods[before]==pressed);held=pressed++;at=t;}else{assert(held>=0 && t-at>=40);held=-1;}}
 }
 assert(pressed==4 && !scheduler_running(&q));
 assert(scheduler_start_all(&q));
 for(unsigned i=0;i<4;i++)assert(q.engines[i].running && q.engines[i].index==0 && q.engines[i].completed==0);
 scheduler_stop_all(&q);assert(!scheduler_running(&q));
 q.engines[2].connected=false;assert(!scheduler_start_all(&q));assert(!scheduler_running(&q));
 puts("PASS: BOOT start-all resets all four, stop-all pauses all, disconnected start is atomic");
 puts("PASS: four simultaneous logics serialize by priority, no overlapping modifiers, finite completion");
 // A long wait in priority 1 must not block priority 2.
 c[0].count=2;c[0].actions[0]=(action_t){.is_wait=true,.wait_ms=86400000};c[0].actions[1]=c[1].actions[0];
 scheduler_init(&q,c);sink=(sink_t){0};scheduler_tick(&q,0,true,true,emit,&sink);
 assert(engine_start(&q.engines[0]));assert(engine_start(&q.engines[1]));
 scheduler_tick(&q,1,true,true,emit,&sink);scheduler_tick(&q,2,true,true,emit,&sink);
 assert(sink.n==1 && sink.key[0]==31 && q.owner==1);
 // Stopping a different logic must never release the active owner's key.
 engine_stop(&q.engines[0]);scheduler_tick(&q,3,true,true,emit,&sink);assert(sink.n==1 && q.owner==1);
 scheduler_stop_all(&q);sink.reject=true;scheduler_tick(&q,4,true,true,emit,&sink);assert(q.owner==1 && q.engines[1].held);
 sink.reject=false;scheduler_tick(&q,5,true,true,emit,&sink);assert(q.owner==-1 && sink.key[1]==0 && !scheduler_running(&q));
 assert(engine_start(&q.engines[1]));scheduler_tick(&q,6,true,true,emit,&sink);scheduler_tick(&q,7,true,true,emit,&sink);
 scheduler_tick(&q,8,false,false,emit,&sink);scheduler_tick(&q,9,true,true,emit,&sink);assert(!scheduler_running(&q) && q.owner==-1);
 puts("PASS: independent 24h wait, stop isolation, failed-release retry, stop-all, reconnect paused");
}
static void check_enabled(void) {
 sequence_t c[4];for(unsigned i=0;i<4;i++){sequence_defaults(&c[i]);c[i].enabled=(i==0||i==2);}
 scheduler_t q;sink_t sink={0};scheduler_init(&q,c);scheduler_tick(&q,0,true,true,emit,&sink);
 assert(scheduler_start_all(&q));assert(q.engines[0].running && q.engines[2].running);
 assert(!q.engines[1].running && !q.engines[3].running && !engine_start(&q.engines[1]));
 q.engines[0].completed=7;assert(scheduler_start_all(&q));assert(q.engines[0].completed==7);
 scheduler_stop_all(&q);for(unsigned i=0;i<4;i++)q.engines[i].config.enabled=false;
 assert(!scheduler_start_all(&q) && !scheduler_running(&q));
 char error[160];sequence_t out;c[0].enabled=false;char *text=config_encode(&c[0]);
 assert(config_parse(text,&out,error,sizeof(error)) && !out.enabled);free(text);
 assert(config_parse("{\"loops\":0,\"actions\":[{\"type\":\"wait\",\"wait\":1}]}",&out,error,sizeof(error)) && out.enabled);
 assert(!config_parse("{\"enabled\":1,\"loops\":0,\"actions\":[{\"type\":\"wait\",\"wait\":1}]}",&out,error,sizeof(error)));
 puts("PASS: disabled logics skipped, single start blocked, all-disabled no-op, running logic not reset, enabled JSON persistence and legacy migration");
}
int main(void){check_sequences();check_config();check_scheduler();check_enabled();return 0;}
