// Inspect the actual share sheet cells while the native sheet is visible.
#import <UIKit/UIKit.h>
static void HKInspectImages(UIView *view, NSMutableArray *rows) {
    if ([view isKindOfClass:UIImageView.class]) {
        UIImage *icon=((UIImageView *)view).image;
        NSMutableDictionary *row=[@{@"class":NSStringFromClass(view.class),@"hidden":@(view.hidden),@"alpha":@(view.alpha),@"rect":NSStringFromCGRect(view.frame),@"hasImage":@(icon!=nil)} mutableCopy];
        if(icon && icon.size.width>0 && icon.size.height>0) {
            uint8_t pixels[32*32*4]={0};
            CGColorSpaceRef space=CGColorSpaceCreateDeviceRGB();
            CGContextRef ctx=CGBitmapContextCreate(pixels,32,32,8,128,space,kCGImageAlphaPremultipliedLast|kCGBitmapByteOrder32Big);
            if(ctx) {
                UIGraphicsPushContext(ctx);[icon drawInRect:CGRectMake(0,0,32,32)];UIGraphicsPopContext();
                unsigned visible=0;for(unsigned i=0;i<1024;i++)visible+=pixels[i*4+3]>0;
                row[@"nontransparentSamples"]=@(visible);CGContextRelease(ctx);
            }
            CGColorSpaceRelease(space);
        }
        [rows addObject:row];
    }
    for(UIView *child in view.subviews) HKInspectImages(child,rows);
}
static void HKInspectCells(UIView *view,NSMutableDictionary *report) {
    if([view isKindOfClass:UILabel.class]) {
        NSString *label=((UILabel *)view).text;
        if([label isEqual:@"AirDrop"] || [label isEqual:@"Mail"]) {
            UIView *cell=view;
            while(cell && ![cell isKindOfClass:UICollectionViewCell.class])cell=cell.superview;
            if(cell){NSMutableArray *rows=[NSMutableArray array];HKInspectImages(cell,rows);
                report[label]=@{@"cell":NSStringFromClass(cell.class),@"images":rows};}
        }
    }
    for(UIView *child in view.subviews) HKInspectCells(child,report);
}
static void HKRecordIcons(UIViewController *sheet) {
    NSMutableDictionary *report=[NSMutableDictionary dictionary];
    HKInspectCells(sheet.view,report);
    NSData *json=[NSJSONSerialization dataWithJSONObject:report options:0 error:nil];
    [json writeToFile:[NSTemporaryDirectory() stringByAppendingPathComponent:@"airdrop-icons.json"] atomically:YES];
}
