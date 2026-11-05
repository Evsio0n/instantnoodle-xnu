/* Include after download_len declaration. Uses only GPT PartitionInfo+BlockIo reads. */
#include "fw_reader.h"
static struct {
 efi_block_io *bio;
 uint32_t media_id, block_size, io_align;
 uint64_t last_block;
} fw_binding;
static int fw_uefi_read(void *opaque,uint64_t lba,uint64_t bytes,void *dst) {
 efi_block_io *bio=(efi_block_io *)opaque;
 if(!bio || !bio->media || !bio->read_blocks || !bio->media->present || !bio->media->logical ||
    bio->media->media_id!=fw_binding.media_id || bio->media->block_size!=fw_binding.block_size ||
    bio->media->last_block!=fw_binding.last_block || bio->media->io_align!=fw_binding.io_align)return 0;
 return !EFI_ERROR(bio->read_blocks(bio,fw_binding.media_id,lba,bytes,dst));
}
static int fw_gpt_name(const efi_partition_info *info,const char *want) {
 unsigned i=0;
 for(;i<36 && want[i];i++)if(info->name[i]!=(uint8_t)want[i])return 0;
 if(i==36)return 0;
 for(;i<36;i++)if(info->name[i])return 0;
 return 1;
}
static int fw_lookup(const char *want,struct fw_device *d) {
 static const efi_guid part_guid=EFI_PARTITION_INFO_GUID, blk_guid=EFI_BLOCK_IO_GUID;
 efi_handle *handles=NULL;uint64_t count=0;efi_block_io *found=NULL;int ambiguous=0;
 if(!fw_allowed(want) || EFI_ERROR(BS->locate_handle_buffer(ByProtocol,&part_guid,NULL,&count,&handles)))return 0;
 for(uint64_t i=0;i<count;i++) {
  efi_partition_info *info;efi_block_io *bio;
  if(EFI_ERROR(BS->handle_protocol(handles[i],&part_guid,(void **)&info)) || !info || info->type!=2 ||
     !fw_gpt_name(info,want) || EFI_ERROR(BS->handle_protocol(handles[i],&blk_guid,(void **)&bio)) ||
     !bio || !bio->media || !bio->media->logical)continue;
  if(found){ambiguous=1;break;}found=bio;
 }
 BS->free_pool(handles);
 if(!found || ambiguous)return 0;
 fw_binding.bio=found;fw_binding.media_id=found->media->media_id;
 fw_binding.block_size=found->media->block_size;fw_binding.io_align=found->media->io_align;
 fw_binding.last_block=found->media->last_block;
 d->last_block=fw_binding.last_block;d->block_size=fw_binding.block_size;d->io_align=fw_binding.io_align;
 d->present=found->media->present;d->logical=found->media->logical;d->opaque=found;d->read=fw_uefi_read;
 return 1;
}
static void fw_hex(char *out,uint64_t v,unsigned digits) {
 for(unsigned i=0;i<digits;i++)out[i]="0123456789abcdef"[(v>>(4*(digits-1-i)))&15];
}
static void fw_command(const char *cmd,int busy) {
 struct fw_request r;uint64_t bytes=0,got=0;uint32_t bs=0;const char *why;
 char reply[46]="size=0000000000000000 block=00000000";
 /* Busy rejection must not mutate the kernel's payload/log/download state. */
 if(busy){fb_reply("FAIL","kernel busy");return;}
 /* Every idle fw request invalidates pending install/download and stale staging. */
 download_len=0;download_to_ramdisk=0;staged_data=out_buf;staged_len=0;
 why=fw_parse(cmd,&r);
 if(!why)why=fw_execute(&r,0,fw_lookup,(void *)(uintptr_t)PAYLOAD_BASE,
     PAYLOAD_MAX_SIZE>0x100000 ? PAYLOAD_MAX_SIZE-0x100000 : 0,&bytes,&bs,&got);
 if(why){fb_reply("FAIL",why);return;}
 if(r.info){fw_hex(reply+5,bytes,16);fw_hex(reply+28,bs,8);fb_reply("OKAY",reply);return;}
 staged_data=(const char *)(uintptr_t)PAYLOAD_BASE;staged_len=got;fb_reply("OKAY","");
}
