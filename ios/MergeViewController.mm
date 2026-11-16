#import "MergeViewController.h"
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <os/proc.h>
#include "job.hpp"
#include "PerformancePolicy.hpp"
#include <atomic>
#include <memory>

using namespace npm;
using namespace npm::mobile;
namespace fs = std::filesystem;

static NSString *display(const std::string& text) {
    return [NSString stringWithUTF8String:text.c_str()] ?: @"无法显示文件信息";
}
enum Section : NSInteger { Source, Sequences, Options, Actions, SectionCount };
enum class Activity { idle, scanning, merging };

@interface MergeViewController () <UIDocumentPickerDelegate>
@end

@implementation MergeViewController {
    ScanResult _scan;
    std::vector<std::string> _problems;
    std::optional<size_t> _selected;
    bool _manualSelection, _memoryWarning;
    Recommendation _policy;
    NSString *_policyReason;
    std::atomic_uint _rowWorkers;
    std::atomic_bool _stop;
    Activity _activity;
    NSURL *_folder, *_pending, *_staging;
    NSFileCoordinator *_coordinator;
    NSOperationQueue *_queue;
    UIDocumentPickerViewController *_exportPicker;
    UISwitch *_ca;
    UILabel *_status;
    UIProgressView *_progress;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"像素位移合成";
    self.navigationController.navigationBar.prefersLargeTitles = YES;
    _queue = [NSOperationQueue new];
    _queue.maxConcurrentOperationCount = 1;
    _queue.qualityOfService = NSQualityOfServiceUserInitiated;
    _ca = [UISwitch new];
    _ca.on = YES;
    _ca.accessibilityLabel = @"色差矫正";
    _status = [UILabel new];
    _status.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
    _status.adjustsFontForContentSizeCategory = YES;
    _status.numberOfLines = 0;
    _status.textColor = UIColor.secondaryLabelColor;
    _progress = [UIProgressView new];
    _progress.accessibilityLabel = @"合成进度";
    UIStackView *footer = [[UIStackView alloc] initWithArrangedSubviews:@[_progress, _status]];
    footer.axis = UILayoutConstraintAxisVertical;
    footer.spacing = 8;
    footer.layoutMargins = UIEdgeInsetsMake(8, 20, 12, 20);
    footer.layoutMarginsRelativeArrangement = YES;
    self.tableView.tableFooterView = footer;
    self.tableView.rowHeight = UITableViewAutomaticDimension;
    self.tableView.estimatedRowHeight = 56;
    [self restorePending];
    [self setStatus:_pending ? @"已有合成文件，可继续导出。" : @"请选择包含 Nikon Z 8 像素位移 NEF 的文件夹。" progress:-1];
    NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
    for (NSNotificationName name in @[NSProcessInfoThermalStateDidChangeNotification, NSProcessInfoPowerStateDidChangeNotification])
        [nc addObserver:self selector:@selector(conditionsChanged:) name:name object:nil];
    [nc addObserver:self selector:@selector(enteredBackground:) name:UIApplicationDidEnterBackgroundNotification object:nil];
    [nc addObserver:self selector:@selector(becameActive:) name:UIApplicationDidBecomeActiveNotification object:nil];
    [self refreshPolicy];
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    UIView *footer = self.tableView.tableFooterView;
    CGFloat width = CGRectGetWidth(self.tableView.bounds);
    CGSize size = [footer systemLayoutSizeFittingSize:CGSizeMake(width, 0)
                       withHorizontalFittingPriority:UILayoutPriorityRequired verticalFittingPriority:UILayoutPriorityFittingSizeLevel];
    if (fabs(CGRectGetHeight(footer.frame) - size.height) > 0.5 || CGRectGetWidth(footer.frame) != width) {
        footer.frame = CGRectMake(0, 0, width, size.height);
        self.tableView.tableFooterView = footer;
    }
}

- (void)setStatus:(NSString *)text progress:(float)value {
    _status.text = text;
    _progress.hidden = value < 0;
    if (value >= 0) _progress.progress = value;
    [self.view setNeedsLayout];
}

- (void)restorePending {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSURL *support = [fm URLsForDirectory:NSApplicationSupportDirectory inDomains:NSUserDomainMask].firstObject;
    _staging = [support URLByAppendingPathComponent:@"PendingMerge" isDirectory:YES];
    for (NSURL *url in [fm contentsOfDirectoryAtURL:_staging includingPropertiesForKeys:nil options:0 error:nil]) {
        if ([url.pathExtension.lowercaseString isEqual:@"nefx"]) _pending = url;
        else if ([url.lastPathComponent hasPrefix:@".nikon-merge-"]) [fm removeItemAtURL:url error:nil];
    }
}

- (void)refreshPolicy {
    NSProcessInfo *pi = NSProcessInfo.processInfo;
    auto state = pi.thermalState;
    Heat heat = Heat::normal;
    if (state == NSProcessInfoThermalStateSerious) heat = Heat::serious;
    if (state == NSProcessInfoThermalStateCritical) heat = Heat::critical;
    Conditions c{os_proc_available_memory(), heat, bool(pi.lowPowerModeEnabled), _memoryWarning};
    _policy = recommend(c);
    // During a run the raster already occupies part of our budget. Only reduce concurrency.
    _rowWorkers = _activity == Activity::merging ? std::min(_rowWorkers.load(), _policy.row_workers) : _policy.row_workers;
    NSMutableArray<NSString *> *reasons = [NSMutableArray new];
    if (_memoryWarning) [reasons addObject:@"系统已发出内存警告"];
    if (heat != Heat::normal) [reasons addObject:heat == Heat::critical ? @"设备温度过高" : @"设备温度偏高"];
    if (c.low_power) [reasons addObject:@"低电量模式已开启"];
    if (!c.available_bytes) [reasons addObject:@"当前内存余量未知或已用尽"];
    else if (c.available_bytes < 1536ull * 1024 * 1024) [reasons addObject:@"当前内存余量偏低"];
    _policyReason = [reasons componentsJoinedByString:@"、"];
    if (!_manualSelection && _activity == Activity::idle)
        _selected = preferred_group(_scan.groups, _problems, _policy);
    [self.tableView reloadData];
}

- (void)conditionsChanged:(NSNotification *)note {
    dispatch_async(dispatch_get_main_queue(), ^{ [self refreshPolicy]; });
}
- (void)becameActive:(NSNotification *)note {
    if (_activity == Activity::idle) _memoryWarning = false;
    [self refreshPolicy];
}
- (void)didReceiveMemoryWarning {
    [super didReceiveMemoryWarning];
    _memoryWarning = true;
    if (self.isViewLoaded) [self refreshPolicy];
}
- (void)enteredBackground:(NSNotification *)note {
    if (_activity != Activity::idle) [self cancelWork];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView { return SectionCount; }
- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    if (section == Sequences) return _scan.groups.size();
    if (section == Actions && _pending && _activity == Activity::idle) return 2;
    return 1;
}

- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section {
    if (section == Sequences && _folder) return @"拍摄序列";
    if (section == Options) return @"合成设置";
    return nil;
}
- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    if (section == Options) {
        NSString *prefix = _policyReason.length ? [_policyReason stringByAppendingString:@"，已降低计算并发。"] : @"";
        return [NSString stringWithFormat:@"%@默认选择最多 %d 帧的完整序列。32 帧可手动选择，成品约 915 MiB。", prefix, _policy.max_default_frames];
    }
    return nil;
}
- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)ip {
    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:@"row"];
    if (!cell) cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:@"row"];
    UIListContentConfiguration *content = cell.defaultContentConfiguration;
    content.textProperties.numberOfLines = 0;
    content.secondaryTextProperties.numberOfLines = 0;
    bool idle = _activity == Activity::idle, enabled = idle;
    cell.accessoryType = UITableViewCellAccessoryNone;
    cell.accessoryView = nil;
    switch (ip.section) {
        case Source:
            content.text = @"选择照片文件夹";
            content.secondaryText = _folder.lastPathComponent;
            cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
            break;
        case Sequences: {
            const auto& group = _scan.groups.at(ip.row);
            const auto& problem = _problems.at(ip.row);
            content.text = [NSString stringWithFormat:@"%d 帧 · %@", group.expected, display(group.frames.front().path.filename().string())];
            content.secondaryText = problem.empty() ? display(group.frames.front().path.parent_path().filename().string()) : display(problem);
            enabled = idle && problem.empty();
            if (_selected && *_selected == size_t(ip.row)) cell.accessoryType = UITableViewCellAccessoryCheckmark;
            break;
        }
        case Options:
            content.text = @"色差矫正";
            cell.accessoryView = _ca;
            _ca.enabled = idle;
            break;
        case Actions:
            if (!idle) {
                content.text = @"取消任务";
                enabled = !_stop.load();
            } else if (_pending) {
                content.text = ip.row == 0 ? @"存储到“文件”…" : @"丢弃待导出文件";
                if (!ip.row) content.secondaryText = _pending.lastPathComponent;
            } else {
                enabled = _selected.has_value();
                content.text = enabled ? [NSString stringWithFormat:@"合成 %d 帧", _scan.groups[*_selected].expected] : @"请先选择完整序列";
            }
            break;
    }
    if (!enabled) content.textProperties.color = UIColor.secondaryLabelColor;
    else if (ip.section == Actions || ip.section == Source)
        content.textProperties.color = ip.section == Actions && _pending && ip.row == 1 ? UIColor.systemRedColor : self.view.tintColor;
    cell.contentConfiguration = content;
    cell.selectionStyle = enabled && ip.section != Options ? UITableViewCellSelectionStyleDefault : UITableViewCellSelectionStyleNone;
    cell.userInteractionEnabled = enabled;
    return cell;
}
- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)ip {
    [tableView deselectRowAtIndexPath:ip animated:YES];
    if (ip.section == Actions && _activity != Activity::idle) { [self cancelWork]; return; }
    if (_activity != Activity::idle) return;
    if (ip.section == Source) {
        UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeFolder] asCopy:NO];
        picker.delegate = self;
        [self presentViewController:picker animated:YES completion:nil];
    } else if (ip.section == Sequences && _problems.at(ip.row).empty()) {
        _manualSelection = true;
        _selected = size_t(ip.row);
        [self refreshPolicy];
    } else if (ip.section == Actions) {
        if (_pending && ip.row == 0) [self exportPending];
        else if (_pending) [self confirmDiscard];
        else if (_selected) [self requestMerge];
    }
}

- (void)cancelWork {
    _stop = true;
    [_coordinator cancel];
    [self setStatus:@"正在取消任务…" progress:-1];
    [self.tableView reloadData];
}

// Keep the selected directory's security scope open until every coordinated read finishes.
- (void)readURLs:(NSArray<NSURL *> *)urls inFolder:(NSURL *)folder
            work:(void (^)(NSArray<NSFileAccessIntent *> *))work
            done:(void (^)(NSString *error))done {
    BOOL scoped = [folder startAccessingSecurityScopedResource];
    NSMutableArray *intents = [NSMutableArray arrayWithCapacity:urls.count];
    for (NSURL *url in urls) [intents addObject:[NSFileAccessIntent readingIntentWithURL:url options:0]];
    _coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
    [_coordinator coordinateAccessWithIntents:intents queue:_queue byAccessor:^(NSError *error) {
        NSString *message = error.localizedDescription;
        if (!error) {
            try {
                if (self->_stop.load()) throw Cancelled{};
                work(intents);
            } catch (const std::bad_alloc&) {
                message = @"内存分配失败。可关闭其他占用内存的任务，或选择较少帧数的拍摄序列。";
            } catch (const std::exception& e) { message = display(e.what()); }
        }
        if (scoped) [folder stopAccessingSecurityScopedResource];
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_coordinator = nil;
            self->_activity = Activity::idle;
            done(message);
            [self refreshPolicy];
        });
    }];
}
- (void)scanFolder:(NSURL *)folder {
    _activity = Activity::scanning;
    _stop = false;
    [self setStatus:@"正在识别拍摄序列…" progress:-1];
    [self.tableView reloadData];
    auto next = std::make_shared<ScanResult>();
    [self readURLs:@[folder] inFolder:folder work:^(NSArray<NSFileAccessIntent *> *items) {
        *next = scan_groups({items.firstObject.URL.fileSystemRepresentation}, [self]{
            if (self->_stop.load()) throw Cancelled{};
        });
        if (self->_stop.load()) throw Cancelled{};
    } done:^(NSString *error) {
        if (error) { [self setStatus:error progress:-1]; return; }
        self->_folder = folder;
        self->_scan = std::move(*next);
        self->_manualSelection = false;
        self->_problems.clear();
        for (const auto& group : self->_scan.groups) self->_problems.push_back(group_problem(group));
        NSString *status = [NSString stringWithFormat:@"找到 %zu 组序列。", self->_scan.groups.size()];
        if (!self->_scan.issues.empty()) {
            const auto& issue = self->_scan.issues.front();
            status = [status stringByAppendingFormat:@"有 %zu 个读取问题。%@：%@", self->_scan.issues.size(),
                      display(issue.path.filename().string()), display(issue.message)];
        }
        [self setStatus:status progress:-1];
    }];
}

- (void)requestMerge {
    // Preserve the intended sequence if conditions changed since the last refresh.
    _manualSelection = true;
    [self refreshPolicy];
    const int frames = _scan.groups[*_selected].expected;
    if (!_policy.confirm(frames)) { [self beginMerge]; return; }
    NSString *condition = _policyReason.length ? [_policyReason stringByAppendingString:@"。"] : @"";
    NSString *cost = frames >= 16 ? @"桌面样片实测峰值内存约 1.2 GiB，成品约 915 MiB。"
                                  : @"桌面样片实测峰值内存约 410 MiB，成品约 232 MiB。";
    NSString *details = [NSString stringWithFormat:@"%@当前建议最多 %d 帧。%@手机内存不足时合成可能中断，请保持应用在前台。",
                         condition, _policy.max_default_frames, cost];
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:[NSString stringWithFormat:@"合成 %d 帧？", frames]
        message:details preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"返回" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"继续合成" style:UIAlertActionStyleDefault handler:^(UIAlertAction *a){ [self beginMerge]; }]];
    [self presentViewController:alert animated:YES completion:nil];
}
- (void)beginMerge {
    if (!_selected || _pending || _activity != Activity::idle) return;
    [self refreshPolicy];
    auto group = std::make_shared<Group>(_scan.groups[*_selected]);
    bool ca = _ca.isOn;
    NSError *error = nil;
    if (![NSFileManager.defaultManager createDirectoryAtURL:_staging withIntermediateDirectories:YES attributes:nil error:&error]) {
        [self setStatus:error.localizedDescription progress:-1]; return;
    }
    [_staging setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:nil];
    NSURL *output = [_staging URLByAppendingPathComponent:display(suggested_filename(*group, ca))];
    NSMutableArray<NSURL *> *urls = [NSMutableArray arrayWithCapacity:group->frames.size()];
    for (const auto& f : group->frames) [urls addObject:[NSURL fileURLWithFileSystemRepresentation:f.path.c_str() isDirectory:NO relativeToURL:nil]];
    _activity = Activity::merging;
    _stop = false;
    [self setStatus:@"准备合成，请保持在前台。" progress:0];
    [self.tableView reloadData];
    [self readURLs:urls inFolder:_folder work:^(NSArray<NSFileAccessIntent *> *items) {
        for (size_t i = 0; i < group->frames.size(); ++i)
            group->frames[i].path = items[i].URL.fileSystemRepresentation;
        int last = -1;
        ExportLimits limits{0, [self]{ return self->_rowWorkers.load(); }};
        export_group(*group, output.fileSystemRepresentation, ca, [&](double fraction, const std::string& stage) {
            if (self->_stop.load()) throw Cancelled{};
            int percent = int(100 * fraction);
            if (percent == last) return;
            last = percent;
            NSString *text = display(stage);
            dispatch_async(dispatch_get_main_queue(), ^{
                if (!self->_stop.load()) [self setStatus:text progress:fraction];
            });
        }, limits);
    } done:^(NSString *message) {
        if (message) [self setStatus:message progress:-1];
        else {
            self->_pending = output;
            [self setStatus:@"合成完成，可存储到“文件”。" progress:1];
        }
    }];
}
- (void)exportPending {
    _exportPicker = [[UIDocumentPickerViewController alloc] initForExportingURLs:@[_pending] asCopy:YES];
    _exportPicker.delegate = self;
    [self presentViewController:_exportPicker animated:YES completion:nil];
}
- (void)clearPending:(NSString *)message {
    NSError *error = nil;
    if ([NSFileManager.defaultManager removeItemAtURL:_pending error:&error]) _pending = nil;
    [self setStatus:error ? error.localizedDescription : message progress:-1];
    [self.tableView reloadData];
}
- (void)confirmDiscard {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"丢弃待导出文件？" message:_pending.lastPathComponent preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"保留" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"丢弃" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *a){ [self clearPending:@"已丢弃待导出文件。"]; }]];
    [self presentViewController:alert animated:YES completion:nil];
}
- (void)documentPicker:(UIDocumentPickerViewController *)picker didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    if (picker == _exportPicker) {
        _exportPicker = nil;
        if (urls.count) [self clearPending:@"已导出 NEFX。"];
    } else if (urls.firstObject) [self scanFolder:urls.firstObject];
}
- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)picker {
    if (picker == _exportPicker) {
        _exportPicker = nil;
        [self setStatus:@"已保留合成文件，可再次导出。" progress:-1];
    }
}
@end
