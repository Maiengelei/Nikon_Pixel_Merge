#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include "job.hpp"
#include "raw_tiff.hpp"
#include <atomic>
#include <memory>

using namespace npm;
namespace fs = std::filesystem;

static NSString *str(const std::string& s) { auto value=[NSString stringWithUTF8String:s.c_str()]; return value ? value : @"（名称无法显示）"; }
static NSTextField *label(NSString *value, CGFloat size, NSFontWeight weight) {
    auto v = [NSTextField labelWithString:value];
    v.font = [NSFont systemFontOfSize:size weight:weight];
    v.lineBreakMode = NSLineBreakByTruncatingMiddle;
    return v;
}
static NSStackView *stack(NSArray<NSView *> *views, bool vertical, CGFloat gap) {
    auto s = [NSStackView stackViewWithViews:views];
    s.orientation = vertical ? NSUserInterfaceLayoutOrientationVertical : NSUserInterfaceLayoutOrientationHorizontal;
    s.alignment = vertical ? NSLayoutAttributeLeading : NSLayoutAttributeCenterY;
    s.spacing = gap;
    return s;
}
@interface Studio : NSObject <NSApplicationDelegate, NSTableViewDataSource, NSTableViewDelegate, NSWindowDelegate>
@end
@implementation Studio {
    NSWindow *_window;
    NSTableView *_table;
    NSCache<NSString*,NSImage*> *_thumbnails;
    NSTextField *_folder, *_details, *_status, *_count;
    NSButton *_browse, *_start, *_ca, *_cancel, *_reveal, *_issues;
    NSProgressIndicator *_progress;
    NSURL *_lastFile;
    ScanResult _scan;
    bool _busy;
    std::shared_ptr<std::atomic_bool> _stop;
}
- (NSButton*)button:(NSString*)title action:(SEL)action {
    return [NSButton buttonWithTitle:title target:self action:action];
}
- (void)applicationDidFinishLaunching:(NSNotification*)note {
    (void)note;
    auto menu = [[NSMenu alloc] init];
    auto appItem = [menu addItemWithTitle:@"" action:nil keyEquivalent:@""];
    auto appMenu = [[NSMenu alloc] init];
    [appMenu addItemWithTitle:@"退出 Nikon Pixel Merge" action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu = appMenu;
    auto fileItem = [menu addItemWithTitle:@"文件" action:nil keyEquivalent:@""];
    auto fileMenu = [[NSMenu alloc] initWithTitle:@"文件"];
    auto openItem = [fileMenu addItemWithTitle:@"选择照片文件夹…" action:@selector(browse:) keyEquivalent:@"o"];
    openItem.target = self;
    fileItem.submenu = fileMenu;
    NSApp.mainMenu = menu;
    _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,800,490)
        styleMask:(NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskMiniaturizable|NSWindowStyleMaskResizable)
        backing:NSBackingStoreBuffered defer:NO];
    _window.title = @"Nikon Pixel Merge";
    _window.delegate = self;
    _window.minSize = NSMakeSize(710,455);

    auto title = label(@"像素位移合并",24,NSFontWeightSemibold);
    _browse = [self button:@"选择照片文件夹…" action:@selector(browse:)];
    _folder = label(@"尚未选择文件夹",12,NSFontWeightRegular);
    _folder.textColor = NSColor.secondaryLabelColor;
    auto importRow = stack(@[_browse,_folder],false,10);
    _count = label(@"拍摄序列",14,NSFontWeightSemibold);
    _issues = [self button:@"查看读取问题…" action:@selector(showIssues:)];
    _issues.hidden = YES;
    auto heading = stack(@[_count,_issues],false,12);
    _table = [[NSTableView alloc] initWithFrame:NSZeroRect];
    NSArray *names = @[@"预览",@"首张照片",@"相机",@"帧数",@"状态"];
    CGFloat widths[] = {64,204,134,76,208};
    _thumbnails=[[NSCache alloc] init]; _thumbnails.countLimit=128;
    for (NSUInteger i=0;i<names.count;i++) {
        auto col = [[NSTableColumn alloc] initWithIdentifier:names[i]];
        col.title = names[i]; col.width=widths[i]; col.minWidth=55;
        [_table addTableColumn:col];
    }
    _table.dataSource=self; _table.delegate=self; _table.rowHeight=42;
    _table.usesAlternatingRowBackgroundColors=YES; _table.allowsMultipleSelection=NO;
    auto scroll = [[NSScrollView alloc] init];
    scroll.documentView=_table; scroll.hasVerticalScroller=YES; scroll.hasHorizontalScroller=YES; scroll.borderType=NSBezelBorder;
    _details = label(@"选择包含 NEF 的文件夹，程序也会检查子文件夹。",12,NSFontWeightRegular);
    _details.textColor = NSColor.secondaryLabelColor;
    _ca=[NSButton checkboxWithTitle:@"色差矫正" target:self action:@selector(selectionUpdated:)];
    _ca.state=NSControlStateValueOn;
    _progress=[[NSProgressIndicator alloc] init];
    _progress.indeterminate=NO; _progress.minValue=0; _progress.maxValue=1;
    _progress.style=NSProgressIndicatorStyleBar;
    _status=label(@"等待选择文件夹",12,NSFontWeightRegular);
    _status.textColor=NSColor.secondaryLabelColor;
    _start=[self button:@"合并并保存…" action:@selector(save:)];
    _start.keyEquivalent=@"\r";
    _cancel=[self button:@"取消" action:@selector(cancel:)];
    _reveal=[self button:@"在访达中显示" action:@selector(reveal:)];
    _reveal.hidden=YES; _cancel.hidden=YES;
    auto buttons=stack(@[_start,_cancel,_reveal],false,8);
    auto main=stack(@[title,importRow,heading,scroll,_details,_ca,_progress,_status,buttons],true,10);
    main.translatesAutoresizingMaskIntoConstraints=NO;
    auto view=_window.contentView; [view addSubview:main];
    [NSLayoutConstraint activateConstraints:@[
        [main.topAnchor constraintEqualToAnchor:view.topAnchor constant:18],
        [main.bottomAnchor constraintEqualToAnchor:view.bottomAnchor constant:-18],
        [main.leadingAnchor constraintEqualToAnchor:view.leadingAnchor constant:22],
        [main.trailingAnchor constraintEqualToAnchor:view.trailingAnchor constant:-22],
        [scroll.widthAnchor constraintEqualToAnchor:main.widthAnchor],
        [scroll.heightAnchor constraintGreaterThanOrEqualToConstant:105],
        [_progress.widthAnchor constraintEqualToAnchor:main.widthAnchor],
        [_details.widthAnchor constraintEqualToAnchor:main.widthAnchor],
        [_status.widthAnchor constraintEqualToAnchor:main.widthAnchor],
        [importRow.widthAnchor constraintLessThanOrEqualToAnchor:main.widthAnchor]
    ]];
    [scroll setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationVertical];
    [self refresh];
    [_window center]; [_window makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES];
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app { (void)app; return YES; }
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)app {
    (void)app;
    if (_busy && _stop) { [self cancel:nil]; return NSTerminateCancel; }
    return NSTerminateNow;
}
- (BOOL)windowShouldClose:(NSWindow*)sender {
    (void)sender;
    if (_busy && _stop) { [self cancel:nil]; return NO; }
    return YES;
}
- (void)message:(NSString*)title detail:(NSString*)detail {
    auto a=[[NSAlert alloc] init]; a.messageText=title; a.informativeText=detail;
    [a beginSheetModalForWindow:_window completionHandler:nil];
}
- (NSInteger)numberOfRowsInTableView:(NSTableView*)view { (void)view; return (NSInteger)_scan.groups.size(); }
- (NSView*)tableView:(NSTableView*)view viewForTableColumn:(NSTableColumn*)column row:(NSInteger)row {
    const auto& g=_scan.groups.at(row);
    if ([column.identifier isEqualToString:@"预览"]) {
        auto thumb=(NSImageView*)[view makeViewWithIdentifier:column.identifier owner:nil];
        if (!thumb) {
            thumb=[[NSImageView alloc] init]; thumb.identifier=column.identifier;
            thumb.imageScaling=NSImageScaleProportionallyUpOrDown; thumb.toolTip=@"首张 NEF 的相机预览";
        }
        auto key=str(g.frames.front().path.string());
        NSImage* img=[_thumbnails objectForKey:key];
        if (!img) try {
            npm::tiff::Mapping input(g.frames.front().path); auto v=input.view(); auto d=v.root();
            auto pos=v.num(v.tag(d,513)), size=v.num(v.tag(d,514));
            if (size && size<=1024*1024) {
                auto data=[NSData dataWithBytes:v.get(pos,size) length:size];
                img=[[NSImage alloc] initWithData:data];
                if (img) {
                    // TIFF orientation also applies to the unrotated embedded JPEG.
                    auto orient=v.num(v.tag(d,274));
                    if (orient==3 || orient==6 || orient==8) {
                        CGFloat angle=orient==3 ? 180 : (orient==6 ? -90 : 90);
                        auto src=img; auto os=src.size;
                        auto ns=orient==3 ? os : NSMakeSize(os.height,os.width);
                        img=[NSImage imageWithSize:ns flipped:NO drawingHandler:^BOOL(NSRect rect) {
                            [NSGraphicsContext saveGraphicsState];
                            auto xform=[NSAffineTransform transform]; [xform translateXBy:NSMidX(rect) yBy:NSMidY(rect)];
                            [xform rotateByDegrees:angle]; [xform concat];
                            [src drawInRect:NSMakeRect(-os.width/2,-os.height/2,os.width,os.height)];
                            [NSGraphicsContext restoreGraphicsState]; return YES;
                        }];
                    }
                    [_thumbnails setObject:img forKey:key];
                }
            }
        } catch (const std::exception&) { /* A missing camera preview does not prevent merging. */ }
        thumb.image=img;
        return thumb;
    }
    auto cell=(NSTextField*)[view makeViewWithIdentifier:column.identifier owner:nil];
    if (!cell) { cell=label(@"",13,NSFontWeightRegular); cell.identifier=column.identifier; }
    if ([column.identifier isEqualToString:@"首张照片"]) cell.stringValue=str(g.frames.front().path.filename().string());
    else if ([column.identifier isEqualToString:@"相机"]) cell.stringValue=str(g.model);
    else if ([column.identifier isEqualToString:@"帧数"]) cell.stringValue=[NSString stringWithFormat:@"%zu / %d",g.frames.size(),g.expected];
    else { auto reason=group_problem(g); cell.stringValue=reason.empty()?@"可合并":str(reason); }
    cell.toolTip=cell.stringValue;
    return cell;
}
- (void)tableViewSelectionDidChange:(NSNotification*)note { (void)note; [self refresh]; }
- (void)selectionUpdated:(id)sender { (void)sender; [self refresh]; }
- (void)refresh {
    const auto row=_table.selectedRow;
    bool selected=row>=0 && (size_t)row<_scan.groups.size();
    std::string problem;
    if (selected) {
        const auto& g=_scan.groups[row]; problem=group_problem(g);
        _details.stringValue=problem.empty()?str(g.frames.front().path.parent_path().filename().string()+"   ·   "+g.frames.front().lens+"   ·   "+(g.expected>=16?"16512 × 11008":"8256 × 5504")):str(problem);
        _details.toolTip=str(g.frames.front().path.string());
    } else { _details.stringValue=@"选择包含 NEF 的文件夹，程序也会检查子文件夹。"; _details.toolTip=nil; }
    _browse.enabled=!_busy; _table.enabled=!_busy; _ca.enabled=!_busy && selected;
    _start.enabled=!_busy && selected && problem.empty();
    _cancel.hidden=!(_busy && _stop); _cancel.enabled=(_stop && !*_stop);
}
- (void)browse:(id)sender {
    (void)sender;
    if (_busy) return;
    auto panel=[NSOpenPanel openPanel]; panel.canChooseDirectories=YES; panel.canChooseFiles=NO;
    panel.prompt=@"扫描"; panel.message=@"选择包含尼康像素位移序列的文件夹";
    [panel beginSheetModalForWindow:_window completionHandler:^(NSModalResponse response) {
        if (response!=NSModalResponseOK) return;
        NSURL* url=panel.URL; if (!url) return;
        self->_busy=true; self->_reveal.hidden=YES; self->_lastFile=nil;
        self->_status.stringValue=@"正在识别拍摄序列…"; self->_folder.stringValue=url.path;
        self->_progress.doubleValue=0; self->_progress.indeterminate=YES; [self->_progress startAnimation:nil];
        [self refresh];
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0), ^{
            auto next=std::make_shared<ScanResult>();
            NSString *error=nil;
            try { *next=scan_groups({fs::path(url.fileSystemRepresentation)}); }
            catch (const std::exception& e) { error=str(e.what()); }
            dispatch_async(dispatch_get_main_queue(), ^{
                self->_scan=std::move(*next); [self->_thumbnails removeAllObjects]; self->_busy=false;
                [self->_progress stopAnimation:nil]; self->_progress.indeterminate=NO;
                self->_count.stringValue=[NSString stringWithFormat:@"拍摄序列（%zu）",self->_scan.groups.size()];
                self->_issues.hidden=self->_scan.issues.empty();
                [self->_table reloadData];
                if (!self->_scan.groups.empty()) {
                    NSUInteger row=0;
                    for (NSUInteger i=0;i<self->_scan.groups.size();i++) if (group_problem(self->_scan.groups[i]).empty()) { row=i; break; }
                    [self->_table selectRowIndexes:[NSIndexSet indexSetWithIndex:row] byExtendingSelection:NO];
                }
                self->_status.stringValue=error ? error : [NSString stringWithFormat:@"识别到 %zu 组；普通 NEF：%zu 张；读取问题：%zu 个。",
                    self->_scan.groups.size(),(size_t)self->_scan.ordinary_nefs,self->_scan.issues.size()];
                [self refresh];
            });
        });
    }];
}
- (void)showIssues:(id)sender {
    (void)sender;
    NSMutableArray<NSString*>* lines=[NSMutableArray array];
    for (const auto& issue:_scan.issues) [lines addObject:str(issue.path.string()+"： "+issue.message)];
    auto text=[[NSTextView alloc] initWithFrame:NSMakeRect(0,0,480,180)];
    text.editable=NO; text.string=[lines componentsJoinedByString:@"\n"];
    auto scroll=[[NSScrollView alloc] initWithFrame:NSMakeRect(0,0,480,180)];
    scroll.hasVerticalScroller=YES; scroll.documentView=text;
    auto alert=[[NSAlert alloc] init]; alert.messageText=@"未能读取的文件"; alert.accessoryView=scroll;
    [alert beginSheetModalForWindow:_window completionHandler:nil];
}
- (void)save:(id)sender {
    (void)sender;
    NSInteger row=_table.selectedRow;
    if (!_start.enabled || row<0 || (size_t)row>=_scan.groups.size()) return;
    auto group=std::make_shared<Group>(_scan.groups[row]);
    bool ca=_ca.state==NSControlStateValueOn;
    auto panel=[NSSavePanel savePanel]; panel.canCreateDirectories=YES;
    panel.allowedContentTypes=@[[UTType typeWithFilenameExtension:@"nefx"]];
    panel.nameFieldStringValue=str(suggested_filename(*group,ca));
    panel.message=@"选择一个新的文件名，已有文件将保留。";
    [panel beginSheetModalForWindow:_window completionHandler:^(NSModalResponse response) {
        if (response!=NSModalResponseOK || !panel.URL) return;
        NSURL *url=panel.URL;
        self->_busy=true; self->_reveal.hidden=YES; self->_lastFile=nil;
        auto stop=std::make_shared<std::atomic_bool>(false); self->_stop=stop;
        self->_status.stringValue=@"准备合并…"; self->_progress.doubleValue=0; [self refresh];
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0), ^{
            NSString* error=nil;
            try {
                export_group(*group,url.fileSystemRepresentation,ca,[=](double p,const std::string& stage){
                    if (*stop) throw Cancelled{};
                    NSString *s=str(stage);
                    dispatch_async(dispatch_get_main_queue(), ^{
                        self->_progress.doubleValue=p; self->_status.stringValue=s;
                    });
                });
            } catch (const std::exception& e) { error=str(e.what()); }
            dispatch_async(dispatch_get_main_queue(), ^{
                self->_busy=false; self->_stop.reset();
                if (error) {
                    self->_status.stringValue=error;
                    self->_progress.doubleValue=0;
                    if (!*stop) [self message:@"未能保存合并图" detail:error];
                } else {
                    self->_status.stringValue=[@"已保存：" stringByAppendingString:url.lastPathComponent];
                    self->_progress.doubleValue=1; self->_lastFile=url; self->_reveal.hidden=NO;
                }
                [self refresh];
            });
        });
    }];
}
- (void)cancel:(id)sender { (void)sender; if (_stop) *_stop=true; _cancel.enabled=NO; _status.stringValue=@"正在取消…"; }
- (void)reveal:(id)sender {
    (void)sender; if (_lastFile) [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[_lastFile]];
}
@end
int main() {
    @autoreleasepool {
        NSApplication* app=[NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        __attribute__((objc_precise_lifetime)) Studio* delegate=[[Studio alloc] init];
        app.delegate=delegate; [app run]; (void)delegate;
    }
    return 0;
}
