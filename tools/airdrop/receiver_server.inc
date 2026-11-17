/* The 20F66 receiver publishes through native CoreP2P, not the public
 * DNSServiceRegister entry in sharingd. Observe the real server, without
 * retaining it or changing its state. UUID/IMP/type guards are mandatory. */
static BOOL (*receiver_everyone)(id,SEL);
static bool receiver_layout_ready;
static ptrdiff_t receiver_http_offset,receiver_screen_offset;
static void receiver_refresh(id self){
 if(!receiver_cfg || !receiver_layout_ready || !receiver_everyone)return;
 void *http=NULL;unsigned char screen=0;
 const unsigned char *bytes=(__bridge const void *)self;
 memcpy(&http,bytes+receiver_http_offset,sizeof(http));
 memcpy(&screen,bytes+receiver_screen_offset,sizeof(screen));
 BOOL everyone=receiver_everyone(self,sel_registerName("discoverableByEveryone"));
 receiver_owner_update((__bridge const void*)self,http && screen && everyone);
 static unsigned state_receipts;if(state_receipts++<32)record("receiver_state http=%u screen=%u everyone=%u",http!=NULL,screen!=0,everyone);
}
static void (*receiver_forward_http)(id,SEL,unsigned short);
static void receiver_http(id self,SEL sel,unsigned short port){receiver_forward_http(self,sel,port);receiver_refresh(self);}
static void (*receiver_forward_state)(id,SEL);
static void receiver_state(id self,SEL sel){receiver_forward_state(self,sel);receiver_refresh(self);}
static void (*receiver_forward_screen)(id,SEL,id);
static void receiver_screen(id self,SEL sel,id note){receiver_forward_screen(self,sel,note);receiver_refresh(self);}
static void (*receiver_forward_mode)(id,SEL,id);
static void receiver_mode(id self,SEL sel,id note){receiver_forward_mode(self,sel,note);receiver_refresh(self);}
static void (*receiver_forward_unpublished)(id,SEL,id);
static void receiver_unpublished(id self,SEL sel,id note){receiver_owner_update((__bridge const void*)self,false);receiver_forward_unpublished(self,sel,note);}
static void (*receiver_forward_stop)(id,SEL);
static void receiver_stop(id self,SEL sel){receiver_owner_update((__bridge const void*)self,false);receiver_forward_stop(self,sel);}
static void (*receiver_forward_dealloc)(__unsafe_unretained id,SEL);
static void receiver_dealloc(__unsafe_unretained id self,SEL sel){receiver_owner_update((__bridge const void*)self,false);receiver_forward_dealloc(self,sel);}
static bool receiver_server_install(void){
 if(!receiver_cfg)return true;
 Class cls=objc_getClass("SDAirDropServer");unsigned count=0;Ivar *ivars=class_copyIvarList(cls,&count);
 bool http=false;for(unsigned i=0;i<count;i++){
  const char *type=ivar_getTypeEncoding(ivars[i]);
  if(type && !strcmp(type,"^{_CFHTTPServer=}") && ivar_getOffset(ivars[i])==72){receiver_http_offset=72;http=true;}
 }
 free(ivars);Ivar screen=class_getInstanceVariable(cls,"_screenOn");
 bool layout=http && screen && ivar_getOffset(screen)==8 && class_getInstanceSize(cls)>=80;
 if(layout)receiver_screen_offset=8;
 receiver_everyone=(void*)install_for_class("SDAirDropServer","discoverableByEveryone","B16@0:8",0x1000b3f68,NULL);
 if(!layout || !receiver_everyone){record("receiver_layout_rejected layout=%u getter=%u",layout,receiver_everyone!=NULL);return false;}
 receiver_forward_http=(void*)install_for_class("SDAirDropServer","startHTTPServer:","v20@0:8S16",0x1000b4b54,(IMP)receiver_http);
 receiver_forward_state=(void*)install_for_class("SDAirDropServer","updateServerState","v16@0:8",0x1000b43c0,(IMP)receiver_state);
 receiver_forward_screen=(void*)install_for_class("SDAirDropServer","screenStateChange:","v24@0:8@16",0x100009b74,(IMP)receiver_screen);
 receiver_forward_mode=(void*)install_for_class("SDAirDropServer","discoverableModeChanged:","v24@0:8@16",0x1000b3e24,(IMP)receiver_mode);
 receiver_forward_unpublished=(void*)install_for_class("SDAirDropServer","airDropUnpublished:","v24@0:8@16",0x1000b4640,(IMP)receiver_unpublished);
 receiver_forward_stop=(void*)install_for_class("SDAirDropServer","stop","v16@0:8",0x1000b5298,(IMP)receiver_stop);
 receiver_forward_dealloc=(void*)install_for_class("SDAirDropServer","dealloc","v16@0:8",0x1000b2fa0,(IMP)receiver_dealloc);
 receiver_layout_ready=receiver_forward_http && receiver_forward_state && receiver_forward_screen && receiver_forward_mode && receiver_forward_unpublished && receiver_forward_stop && receiver_forward_dealloc;
 record("receiver_layout_verified ready=%u",receiver_layout_ready);return receiver_layout_ready;
}
