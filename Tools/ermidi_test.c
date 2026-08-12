#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ER_VID 0x0DBA
#define ER_PID 0xB011
#define ER_MIDI_IF 2

static int hexNibble(char c){
    if(c>='0'&&c<='9')return c-'0';
    if(c>='a'&&c<='f')return c-'a'+10;
    if(c>='A'&&c<='F')return c-'A'+10;
    return -1;
}

static size_t encodeSysex(const UInt8 *midi,size_t length,UInt8 *usb,size_t capacity){
    size_t input=0,output=0;
    while(input<length){
        size_t remaining=length-input,count=remaining>=3?3:remaining;
        UInt8 cin=0x04;
        if(midi[input+count-1]==0xF7)cin=count==1?0x05:(count==2?0x06:0x07);
        if(output+4>capacity)return 0;
        usb[output]=cin;usb[output+1]=midi[input];
        usb[output+2]=count>1?midi[input+1]:0;
        usb[output+3]=count>2?midi[input+2]:0;
        output+=4;input+=count;
    }
    return output;
}

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

int main(int argc,char **argv){
    int sendCC=0,allChannels=0,cc=0,value=0,listenSeconds=0,enableGreenJRC=0,sendRawSysex=0;
    UInt8 rawSysex[1024];size_t rawSysexLength=0;
    if(argc==4&&(strcmp(argv[1],"--cc")==0||strcmp(argv[1],"--cc-all")==0)){
        char *ccEnd=NULL,*valueEnd=NULL;
        long parsedCC=strtol(argv[2],&ccEnd,10),parsedValue=strtol(argv[3],&valueEnd,10);
        if(!ccEnd||*ccEnd||!valueEnd||*valueEnd||parsedCC<0||parsedCC>127||parsedValue<0||parsedValue>127){
            fprintf(stderr,"usage: %s [--cc|--cc-all 0-127 0-127] [--listen 1-300] [--enable-green-jrc]\n",argv[0]);return 64;
        }
        sendCC=1;allChannels=strcmp(argv[1],"--cc-all")==0;cc=(int)parsedCC;value=(int)parsedValue;
    }else if(argc==3&&strcmp(argv[1],"--listen")==0){
        char *end=NULL;long seconds=strtol(argv[2],&end,10);
        if(!end||*end||seconds<1||seconds>300){
            fprintf(stderr,"usage: %s [--cc|--cc-all 0-127 0-127] [--listen 1-300]\n",argv[0]);return 64;
        }
        listenSeconds=(int)seconds;
    }else if(argc==2&&strcmp(argv[1],"--enable-green-jrc")==0){
        enableGreenJRC=1;
    }else if(argc==3&&strcmp(argv[1],"--sysex")==0){
        size_t hexLength=strlen(argv[2]);
        if((hexLength&1)||hexLength<4||hexLength>sizeof(rawSysex)*2){
            fprintf(stderr,"SysEx must be an even-length hexadecimal string\n");return 64;
        }
        for(size_t i=0;i<hexLength;i+=2){
            int high=hexNibble(argv[2][i]),low=hexNibble(argv[2][i+1]);
            if(high<0||low<0){fprintf(stderr,"invalid hexadecimal SysEx\n");return 64;}
            rawSysex[rawSysexLength++]=(UInt8)((high<<4)|low);
        }
        if(rawSysex[0]!=0xF0||rawSysex[rawSysexLength-1]!=0xF7){
            fprintf(stderr,"SysEx must begin with F0 and end with F7\n");return 64;
        }
        sendRawSysex=1;
    }else if(argc!=1){
        fprintf(stderr,"usage: %s [--cc|--cc-all 0-127 0-127] [--listen 1-300] [--enable-green-jrc]\n",argv[0]);return 64;
    }
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

    if(sendRawSysex){
        UInt8 usb[2048];size_t usbLength=encodeSysex(rawSysex,rawSysexLength,usb,sizeof(usb));
        if(!usbLength){puts("could not encode SysEx");return 7;}
        r=(*midi)->WritePipeTO(midi,outPipe,usb,(UInt32)usbLength,1000,1000);
        printf("raw SysEx write (%zu MIDI bytes): 0x%x%s\n",rawSysexLength,r,r?" FAILED":" OK");
        if(!r){
            static const UInt8 cinLength[16]={0,0,2,3,3,1,2,3,3,3,3,3,2,2,3,1};
            UInt8 responseSysex[8192];size_t responseLength=0;int inSysex=0;
            CFAbsoluteTime deadline=CFAbsoluteTimeGetCurrent()+3.0;
            while(CFAbsoluteTimeGetCurrent()<deadline){
                UInt8 input[512]={0};UInt32 inputLength=sizeof(input);
                IOReturn readResult=(*midi)->ReadPipeTO(midi,inPipe,input,&inputLength,250,250);
                if(readResult==kIOReturnTimeout||readResult==kIOUSBTransactionTimeout)continue;
                if(readResult)break;
                for(UInt32 offset=0;offset+3<inputLength;offset+=4){
                    UInt8 count=cinLength[input[offset]&0x0F];
                    for(UInt8 index=0;index<count;index++){
                        UInt8 byte=input[offset+1+index];
                        if(!inSysex&&byte==0xF0){inSysex=1;responseLength=0;}
                        if(inSysex&&responseLength<sizeof(responseSysex))responseSysex[responseLength++]=byte;
                        if(inSysex&&byte==0xF7){
                            printf("response (%zu bytes):",responseLength);
                            for(size_t i=0;i<responseLength;i++)printf(" %02X",responseSysex[i]);
                            putchar('\n');fflush(stdout);inSysex=0;responseLength=0;
                        }
                    }
                }
            }
        }
        (*midi)->USBInterfaceClose(midi);(*midi)->Release(midi);
        (*dev)->USBDeviceClose(dev);(*dev)->Release(dev);
        return r?7:0;
    }

    if(enableGreenJRC){
        /* Captured Green JRC control tuple: instance 4, control 1.  Change
           function 0x02 (asynchronous device update) to 0x00 (host Set).
           The five-byte value is the captured high/on control value. */
        const UInt8 packet[20]={
            0x04,0xF0,0x13,0x0B,
            0x04,0x0F,0x00,0x11,
            0x04,0x04,0x01,0x40,
            0x04,0x00,0x00,0x00,
            0x06,0x10,0xF7,0x00
        };
        r=(*midi)->WritePipeTO(midi,outPipe,(void*)packet,sizeof(packet),1000,1000);
        printf("Green JRC enable SysEx write: 0x%x%s\n",r,r?" FAILED":" OK");
        (*midi)->USBInterfaceClose(midi);(*midi)->Release(midi);
        (*dev)->USBDeviceClose(dev);(*dev)->Release(dev);
        return r?7:0;
    }

    if(listenSeconds){
        static const UInt8 cinLength[16]={0,0,2,3,3,1,2,3,3,3,3,3,2,2,3,1};
        UInt8 sysex[8192];size_t sysexLength=0;int inSysex=0;
        CFAbsoluteTime deadline=CFAbsoluteTimeGetCurrent()+listenSeconds;
        printf("READY: listening for %d seconds; toggle Green JRC once now.\n",listenSeconds);fflush(stdout);
        while(CFAbsoluteTimeGetCurrent()<deadline){
            UInt8 usb[512]={0};UInt32 length=sizeof(usb);
            r=(*midi)->ReadPipeTO(midi,inPipe,usb,&length,250,250);
            if(r==kIOReturnTimeout||r==kIOUSBTransactionTimeout)continue;
            if(r){printf("MIDI read failed 0x%x\n",r);break;}
            for(UInt32 offset=0;offset+3<length;offset+=4){
                UInt8 cable=usb[offset]>>4,count=cinLength[usb[offset]&0x0F];
                for(UInt8 index=0;index<count;index++){
                    UInt8 byte=usb[offset+1+index];
                    if(!inSysex&&byte==0xF0){inSysex=1;sysexLength=0;}
                    if(inSysex&&sysexLength<sizeof(sysex))sysex[sysexLength++]=byte;
                    if(inSysex&&byte==0xF7){
                        printf("SysEx cable %u (%zu bytes):",cable,sysexLength);
                        for(size_t i=0;i<sysexLength;i++)printf(" %02X",sysex[i]);
                        putchar('\n');fflush(stdout);inSysex=0;sysexLength=0;
                    }
                }
            }
        }
        if(inSysex)printf("incomplete SysEx (%zu bytes)\n",sysexLength);
        puts("capture complete");
        (*midi)->USBInterfaceClose(midi);(*midi)->Release(midi);
        (*dev)->USBDeviceClose(dev);(*dev)->Release(dev);
        return 0;
    }

    if(sendCC){
        /* USB-MIDI cable 0, CIN 0xB (three-byte Control Change). */
        UInt8 packet[4]={0x0B,0xB0,(UInt8)cc,(UInt8)value};
        int firstChannel=0,lastChannel=allChannels?15:0;
        for(int channel=firstChannel;channel<=lastChannel;channel++){
            packet[1]=(UInt8)(0xB0|channel);
            r=(*midi)->WritePipeTO(midi,outPipe,packet,sizeof(packet),1000,1000);
            printf("control change channel %d CC %d value %d: 0x%x%s\n",
                   channel+1,cc,value,r,r?" FAILED":" OK");
            if(r)break;
        }
        (*midi)->USBInterfaceClose(midi);(*midi)->Release(midi);
        (*dev)->USBDeviceClose(dev);(*dev)->Release(dev);
        return r?7:0;
    }

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
