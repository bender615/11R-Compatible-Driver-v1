#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <stdio.h>
#include <string.h>

#define ER_VID 0x0DBA
#define ER_PID 0xB011
#define ER_MIDI_IF 2

static IOUSBDeviceInterface500 **openDevice(io_service_t service) {
    IOCFPlugInInterface **plugin=NULL; SInt32 score=0;
    if(IOCreatePlugInInterfaceForService(service,kIOUSBDeviceUserClientTypeID,
       kIOCFPlugInInterfaceID,&plugin,&score)!=KERN_SUCCESS||!plugin)return NULL;
    IOUSBDeviceInterface500 **dev=NULL;
    (*plugin)->QueryInterface(plugin,CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID500),(LPVOID*)&dev);
    (*plugin)->Release(plugin); return dev;
}

static IOUSBInterfaceInterface500 **openMidiInterface(IOUSBDeviceInterface500 **dev) {
    IOUSBFindInterfaceRequest req={kIOUSBFindInterfaceDontCare,kIOUSBFindInterfaceDontCare,
                                  kIOUSBFindInterfaceDontCare,kIOUSBFindInterfaceDontCare};
    io_iterator_t iter=0; if((*dev)->CreateInterfaceIterator(dev,&req,&iter))return NULL;
    io_service_t service; IOUSBInterfaceInterface500 **answer=NULL;
    while((service=IOIteratorNext(iter))){
        IOCFPlugInInterface **plugin=NULL; SInt32 score=0;
        if(IOCreatePlugInInterfaceForService(service,kIOUSBInterfaceUserClientTypeID,
           kIOCFPlugInInterfaceID,&plugin,&score)==KERN_SUCCESS&&plugin){
            IOUSBInterfaceInterface500 **candidate=NULL;
            (*plugin)->QueryInterface(plugin,CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID500),(LPVOID*)&candidate);
            (*plugin)->Release(plugin);
            if(candidate){ UInt8 number=255; (*candidate)->GetInterfaceNumber(candidate,&number);
                if(number==ER_MIDI_IF){answer=candidate;IOObjectRelease(service);break;}
                (*candidate)->Release(candidate); }
        }
        IOObjectRelease(service);
    }
    IOObjectRelease(iter); return answer;
}

int main(void){
    SInt32 vid=ER_VID,pid=ER_PID; CFMutableDictionaryRef match=IOServiceMatching(kIOUSBDeviceClassName);
    CFNumberRef v=CFNumberCreate(NULL,kCFNumberSInt32Type,&vid),p=CFNumberCreate(NULL,kCFNumberSInt32Type,&pid);
    CFDictionarySetValue(match,CFSTR(kUSBVendorID),v);CFDictionarySetValue(match,CFSTR(kUSBProductID),p);
    CFRelease(v);CFRelease(p); io_service_t service=IOServiceGetMatchingService(kIOMainPortDefault,match);
    if(!service){puts("Eleven Rack not found");return 1;}
    IOUSBDeviceInterface500 **dev=openDevice(service);IOObjectRelease(service);if(!dev)return 2;
    IOReturn r=(*dev)->USBDeviceOpen(dev); if(r==kIOReturnExclusiveAccess)r=(*dev)->USBDeviceOpenSeize(dev);
    if(r){printf("device open failed 0x%x\n",r);return 3;} (*dev)->SetConfiguration(dev,1);
    IOUSBInterfaceInterface500 **midi=openMidiInterface(dev);if(!midi){puts("MIDI interface 2 not found");return 4;}
    r=(*midi)->USBInterfaceOpen(midi);
    if(r==kIOReturnExclusiveAccess){
        puts("MIDI interface is owned by MIDIServer; seizing it for this isolated test");
        r=(*midi)->USBInterfaceOpenSeize(midi);
    }
    if(r){printf("MIDI interface open failed 0x%x\n",r);return 5;}
    UInt8 count=0,outPipe=0,inPipe=0;(*midi)->GetNumEndpoints(midi,&count);
    for(UInt8 pipe=1;pipe<=count;pipe++){UInt8 direction=0,number=0,transfer=0,interval=0;UInt16 max=0;
        (*midi)->GetPipeProperties(midi,pipe,&direction,&number,&transfer,&max,&interval);
        printf("pipe %u: %s bulk ep %u max %u\n",pipe,direction==kUSBIn?"IN":"OUT",number,max);
        if(transfer==kUSBBulk&&direction==kUSBIn)inPipe=pipe;
        if(transfer==kUSBBulk&&direction==kUSBOut)outPipe=pipe;
    }
    if(!outPipe||!inPipe){puts("bulk MIDI endpoints missing");return 6;}

    const UInt8 identityRequest[8]={0x04,0xF0,0x7E,0x7F, 0x07,0x06,0x01,0xF7};
    r=(*midi)->WritePipeTO(midi,outPipe,(void*)identityRequest,sizeof(identityRequest),1000,1000);
    printf("identity request write: 0x%x%s\n",r,r?" FAILED":" OK");
    UInt8 response[512]={0}; UInt32 length=sizeof(response);
    r=(*midi)->ReadPipeTO(midi,inPipe,response,&length,2000,2000);
    printf("identity response read: 0x%x, %u bytes\n",r,(unsigned)length);
    if(!r&&length){
        printf("USB-MIDI packets:");for(UInt32 i=0;i<length;i++)printf(" %02X",response[i]);puts("");
        printf("decoded MIDI:");
        for(UInt32 i=0;i+3<length;i+=4){UInt8 cin=response[i]&0x0F,n=cin==5?1:(cin==2||cin==6||cin==0xC||cin==0xD?2:3);
            for(UInt8 j=0;j<n;j++)printf(" %02X",response[i+1+j]);}
        puts("");
    }
    (*midi)->USBInterfaceClose(midi);(*midi)->Release(midi);
    (*dev)->USBDeviceClose(dev);(*dev)->Release(dev);
    return (!r&&length)?0:7;
}
