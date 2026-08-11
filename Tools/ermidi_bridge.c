#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ER_VID 0x0DBA
#define ER_PID 0xB011
#define ER_MIDI_IF 2
#define ER_CABLES 2

static IOUSBDeviceInterface500 **gDev;
static IOUSBInterfaceInterface500 **gMidi;
static UInt8 gInPipe,gOutPipe;
static MIDIClientRef gClient;
static MIDIEndpointRef gSources[ER_CABLES],gDestinations[ER_CABLES];
static pthread_t gReader;
static pthread_mutex_t gWriteLock=PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t gStop,gIdentitySeen;

static IOUSBDeviceInterface500 **openDevice(io_service_t service){
    IOCFPlugInInterface **p=NULL;SInt32 score=0;IOUSBDeviceInterface500 **d=NULL;
    if(IOCreatePlugInInterfaceForService(service,kIOUSBDeviceUserClientTypeID,kIOCFPlugInInterfaceID,&p,&score)==KERN_SUCCESS&&p){
        (*p)->QueryInterface(p,CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID500),(LPVOID*)&d);(*p)->Release(p);}
    return d;
}
static IOUSBInterfaceInterface500 **findMidi(IOUSBDeviceInterface500 **d){
    IOUSBFindInterfaceRequest q={kIOUSBFindInterfaceDontCare,kIOUSBFindInterfaceDontCare,kIOUSBFindInterfaceDontCare,kIOUSBFindInterfaceDontCare};
    io_iterator_t it=0;if((*d)->CreateInterfaceIterator(d,&q,&it))return NULL;
    io_service_t s;IOUSBInterfaceInterface500 **answer=NULL;
    while((s=IOIteratorNext(it))){IOCFPlugInInterface **p=NULL;SInt32 score=0;
        if(IOCreatePlugInInterfaceForService(s,kIOUSBInterfaceUserClientTypeID,kIOCFPlugInInterfaceID,&p,&score)==KERN_SUCCESS&&p){
            IOUSBInterfaceInterface500 **x=NULL;(*p)->QueryInterface(p,CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID500),(LPVOID*)&x);(*p)->Release(p);
            if(x){UInt8 n=255;(*x)->GetInterfaceNumber(x,&n);if(n==ER_MIDI_IF){answer=x;IOObjectRelease(s);break;}(*x)->Release(x);}}
        IOObjectRelease(s);}
    IOObjectRelease(it);return answer;
}
static int openUSB(void){
    SInt32 vid=ER_VID,pid=ER_PID;CFMutableDictionaryRef m=IOServiceMatching(kIOUSBDeviceClassName);
    CFNumberRef v=CFNumberCreate(NULL,kCFNumberSInt32Type,&vid),p=CFNumberCreate(NULL,kCFNumberSInt32Type,&pid);
    CFDictionarySetValue(m,CFSTR(kUSBVendorID),v);CFDictionarySetValue(m,CFSTR(kUSBProductID),p);CFRelease(v);CFRelease(p);
    io_service_t s=IOServiceGetMatchingService(kIOMainPortDefault,m);if(!s)return 0;
    gDev=openDevice(s);IOObjectRelease(s);if(!gDev)return 0;
    IOReturn r=(*gDev)->USBDeviceOpen(gDev);if(r==kIOReturnExclusiveAccess)r=(*gDev)->USBDeviceOpenSeize(gDev);if(r)return 0;
    (*gDev)->SetConfiguration(gDev,1);gMidi=findMidi(gDev);if(!gMidi)return 0;
    r=(*gMidi)->USBInterfaceOpen(gMidi);if(r)return 0;
    UInt8 count=0;(*gMidi)->GetNumEndpoints(gMidi,&count);
    for(UInt8 i=1;i<=count;i++){UInt8 dir=0,num=0,type=0,ival=0;UInt16 max=0;(*gMidi)->GetPipeProperties(gMidi,i,&dir,&num,&type,&max,&ival);
        if(type==kUSBBulk&&dir==kUSBIn)gInPipe=i;if(type==kUSBBulk&&dir==kUSBOut)gOutPipe=i;}
    return gInPipe&&gOutPipe;
}
static UInt8 cinLength(UInt8 cin){static const UInt8 n[16]={0,0,2,3,3,1,2,3,3,3,3,3,2,2,3,1};return n[cin&15];}
static void emitEvent(UInt8 cable,UInt8 cin,const UInt8 *b,UInt8 n,UInt8 *out,UInt32 *used){
    if(*used+4>512)return;out[*used]=(cable<<4)|(cin&15);out[*used+1]=n>0?b[0]:0;out[*used+2]=n>1?b[1]:0;out[*used+3]=n>2?b[2]:0;*used+=4;
}
static UInt8 statusLength(UInt8 s){if(s<0x80)return 0;if(s<0xC0)return 3;if(s<0xE0)return 2;if(s<0xF0)return 3;
    if(s==0xF1||s==0xF3)return 2;if(s==0xF2)return 3;return 1;}
static void encodeMIDI(UInt8 cable,const UInt8 *data,UInt32 len,UInt8 *out,UInt32 *used){
    UInt32 i=0;while(i<len){
        if(data[i]==0xF0){UInt8 b[3];UInt8 n=0;b[n++]=data[i++];
            while(i<len){b[n++]=data[i++];if(b[n-1]==0xF7){emitEvent(cable,n==1?5:n==2?6:7,b,n,out,used);n=0;break;}
                if(n==3){emitEvent(cable,4,b,3,out,used);n=0;}}
            if(n)emitEvent(cable,n==1?5:n==2?6:4,b,n,out,used);continue;}
        UInt8 s=data[i],n=statusLength(s);if(!n){i++;continue;}if(i+n>len)n=(UInt8)(len-i);
        UInt8 cin=(s>>4)&15;if(s>=0xF0)cin=n==1?15:n==2?2:3;emitEvent(cable,cin,data+i,n,out,used);i+=n;
    }
}
static void coreMidiOutput(const MIDIPacketList *list,void *ref,void *src){
    (void)src;UInt8 cable=(UInt8)(uintptr_t)ref;UInt8 usb[512];UInt32 used=0;
    const MIDIPacket *p=&list->packet[0];for(UInt32 i=0;i<list->numPackets;i++){encodeMIDI(cable,p->data,p->length,usb,&used);p=MIDIPacketNext(p);}
    if(used){pthread_mutex_lock(&gWriteLock);IOReturn r=(*gMidi)->WritePipeTO(gMidi,gOutPipe,usb,used,1000,1000);pthread_mutex_unlock(&gWriteLock);
        if(r)fprintf(stderr,"MIDI USB write failed 0x%x\n",r);}
}
static void deliver(UInt8 cable,const UInt8 *data,UInt8 n){if(cable>=ER_CABLES||!n)return;
    UInt8 storage[256];MIDIPacketList *l=(MIDIPacketList*)storage;MIDIPacket *p=MIDIPacketListInit(l);MIDIPacketListAdd(l,sizeof(storage),p,0,n,data);MIDIReceived(gSources[cable],l);}
static void *reader(void *unused){(void)unused;UInt8 usb[512];while(!gStop){UInt32 n=sizeof(usb);IOReturn r=(*gMidi)->ReadPipeTO(gMidi,gInPipe,usb,&n,250,250);
        if(r==kIOReturnTimeout||r==kIOUSBTransactionTimeout)continue;if(r){if(!gStop)fprintf(stderr,"MIDI USB read failed 0x%x\n",r);break;}
        for(UInt32 i=0;i+3<n;i+=4){UInt8 cable=usb[i]>>4,count=cinLength(usb[i]);deliver(cable,usb+i+1,count);}}
    return NULL;}
static void selfTestRead(const MIDIPacketList *list,void *ref,void *src){(void)ref;(void)src;const MIDIPacket *p=&list->packet[0];
    for(UInt32 i=0;i<list->numPackets;i++){if(p->length>=6&&p->data[0]==0xF0&&p->data[1]==0x7E&&p->data[3]==0x06&&p->data[4]==0x02)gIdentitySeen=1;p=MIDIPacketNext(p);}}
static void cleanup(void){gStop=1;if(gMidi)(*gMidi)->AbortPipe(gMidi,gInPipe);if(gReader)pthread_join(gReader,NULL);
    if(gClient)MIDIClientDispose(gClient);if(gMidi){(*gMidi)->USBInterfaceClose(gMidi);(*gMidi)->Release(gMidi);}if(gDev){(*gDev)->USBDeviceClose(gDev);(*gDev)->Release(gDev);}}
static void sig(int x){(void)x;gStop=1;CFRunLoopStop(CFRunLoopGetMain());}
int main(int argc,char **argv){int selfTest=argc>1&&!strcmp(argv[1],"--self-test");
    puts("Eleven Rack CoreMIDI bridge");if(!openUSB()){puts("cannot claim MIDI interface 2 (stop MIDIServer and retry)");cleanup();return 1;}
    if(MIDIClientCreate(CFSTR("Eleven Rack MIDI Bridge"),NULL,NULL,&gClient)){puts("CoreMIDI client failed");cleanup();return 2;}
    const CFStringRef names[ER_CABLES]={CFSTR("Eleven Rack Rig"),CFSTR("Eleven Rack External")};
    for(UInt8 i=0;i<ER_CABLES;i++){if(MIDISourceCreate(gClient,names[i],&gSources[i])||MIDIDestinationCreate(gClient,names[i],coreMidiOutput,(void*)(uintptr_t)i,&gDestinations[i])){cleanup();return 3;}}
    pthread_create(&gReader,NULL,reader,NULL);printf("ports online: Eleven Rack Rig, Eleven Rack External\n");
    if(selfTest){MIDIPortRef out=0,in=0;MIDIOutputPortCreate(gClient,CFSTR("self test out"),&out);MIDIInputPortCreate(gClient,CFSTR("self test in"),selfTestRead,NULL,&in);MIDIPortConnectSource(in,gSources[0],NULL);
        UInt8 mem[256],msg[]={0xF0,0x7E,0x7F,0x06,0x01,0xF7};MIDIPacketList *l=(MIDIPacketList*)mem;MIDIPacket *p=MIDIPacketListInit(l);MIDIPacketListAdd(l,sizeof(mem),p,0,sizeof(msg),msg);MIDISend(out,gDestinations[0],l);
        for(int i=0;i<30&&!gIdentitySeen;i++)usleep(100000);printf("end-to-end CoreMIDI identity test: %s\n",gIdentitySeen?"PASS":"FAIL");MIDIPortDispose(in);MIDIPortDispose(out);cleanup();return gIdentitySeen?0:4;}
    signal(SIGINT,sig);signal(SIGTERM,sig);CFRunLoopRun();cleanup();return 0;
}
