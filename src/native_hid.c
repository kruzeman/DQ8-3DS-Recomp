/* Publish real host button input through the guest HID shared-memory ring. */
static void put32(uint32_t, uint32_t);
static uint32_t native_hid_base, native_hid_buttons, native_hid_previous;
static uint32_t native_hid_events[5], native_hid_index;
static uint64_t native_hid_next_update;
static void native_hid_get_handles(void) {
    for(unsigned i=0;i<5;i++)if(!native_hid_events[i]) {
        native_hid_events[i]=guest_alloc_ipc_handle();
        event_live[native_hid_events[i]]=1;
        event_manual_reset[native_hid_events[i]]=0;
    }
    if(getenv("DQ8_TRACE_INPUT"))
        fprintf(stderr,"HID handles sharedmem=%08X pad_events=%08X/%08X accel=%08X gyro=%08X debugpad=%08X\n",
                native_hid_base,native_hid_events[0],native_hid_events[1],
                native_hid_events[2],native_hid_events[3],native_hid_events[4]);
    put32(active_tls_base+0x80,0x000A0047u);
    put32(active_tls_base+0x84,0);
    put32(active_tls_base+0x88,0x14000000u); /* CopyHandleDesc(6) */
    put32(active_tls_base+0x8c,0x30010u);
    for(unsigned i=0;i<5;i++)put32(active_tls_base+0x90+i*4,native_hid_events[i]);
}
/* IDLE THROTTLE hooks (idle block above this marker):
   - idle_note_input_state(): held-button state feeds the detector on every
     call (arming requires no held input).
   - idle_note_input(): a real edge releases the throttle and pulls the
     frame deadline due so the guest reacts immediately.
   - idle_throttle_armed(): while armed an UNCHANGED ring entry no longer
     signals the pad events (that unconditional 60 Hz wake is a per-frame
     idle cost); a real edge always signals, armed or not.  The ring write
     itself keeps its exact cadence, layout and timestamp semantics. */
static void native_hid_pump(void) {
    if(!native_hid_base)return;
    uint64_t now=host_monotonic_ns();
    const char *path=getenv("DQ8_INPUT_FILE");
    if(path && *path) {
        FILE *file=fopen(path,"r");
        if(file) { char text[64];if(fgets(text,sizeof(text),file))native_hid_buttons=(uint32_t)strtoul(text,NULL,0);fclose(file); }
    }
    idle_note_input_state(native_hid_buttons);
    uint32_t changed=native_hid_previous^native_hid_buttons;
    /* A fresh edge bypasses the publish cadence: input delivery latency
       must not stack the full HID period on top of the wake path. */
    if(now<native_hid_next_update && !changed)return;
    trace_hid_runtime_ctx++;
    native_hid_next_update=now+16666667u;
    uint32_t base=native_hid_base,index=native_hid_index++ & 7u;
    uint32_t entry=base+40u+index*16u;
    put32(entry,native_hid_buttons);put32(entry+4,changed & native_hid_buttons);
    put32(entry+8,changed & native_hid_previous);put32(entry+12,0);
    put32(base+28,native_hid_buttons);put32(base+16,index);
    if(index==0) {
        uint64_t ticks=(now/1000000000ull)*268111856ull+(now%1000000000ull)*268111856ull/1000000000ull;
        put32(base+8,mem32(base));put32(base+12,mem32(base+4));
        put32(base,(uint32_t)ticks);put32(base+4,(uint32_t)(ticks>>32));
    }
    if(changed && getenv("DQ8_TRACE_INPUT"))fprintf(stderr,"HID buttons=%08X pressed=%08X released=%08X index=%u t=%.3f\n",native_hid_buttons,changed & native_hid_buttons,changed & native_hid_previous,index,(double)now/1e9);
    native_hid_previous=native_hid_buttons;
    if(changed)idle_note_input();
    for(unsigned i=0;i<2;i++) {
        uint32_t event=native_hid_events[i];if(!event)continue;
        if(!changed && idle_throttle_armed())continue; /* static state: no 60 Hz guest wake */
        event_signaled[event]=1;
        guest_notify_handle(event);
    }
    trace_hid_runtime_ctx--;
}
