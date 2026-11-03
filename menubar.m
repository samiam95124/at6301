/*
 * AT6301 menu bar launcher.
 *
 * Puts a bolt in the Mac menu bar. Its menu opens the AT6301 front panel
 * (on the real supply, or simulated), or brings it forward if it is
 * already open. It runs as a menu bar item only: no Dock icon, no window.
 *
 * The panel program is started from the directory the launcher's bundle
 * was built from (AT6301Dir in Info.plist), so it finds its ami.cfg.
 */
#import <Cocoa/Cocoa.h>

@interface Launcher : NSObject <NSApplicationDelegate, NSMenuDelegate>
@property (strong) NSStatusItem *item;
@property (strong) NSTask *panel;
@property (copy) NSString *dir;
@property (strong) NSMenuItem *openReal, *openSim, *closePanel;
@end

@implementation Launcher

- (void)applicationDidFinishLaunching:(NSNotification *)n
{
    NSMenu *menu;
    NSImage *img;

    self.dir = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"AT6301Dir"];
    self.item = [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];
    img = [NSImage imageWithSystemSymbolName:@"bolt.fill" accessibilityDescription:@"AT6301"];
    if (img) {
        img.template = YES; /* follows light and dark menu bars */
        self.item.button.image = img;
    } else {
        self.item.button.title = @"AT6301";
    }
    self.item.button.toolTip = @"AT6301 power supply";

    menu = [[NSMenu alloc] init];
    menu.delegate = self;
    menu.autoenablesItems = NO;
    self.openReal = [menu addItemWithTitle:@"Open AT6301" action:@selector(openReal:) keyEquivalent:@""];
    self.openSim = [menu addItemWithTitle:@"Open AT6301 (simulated)" action:@selector(openSim:)
                            keyEquivalent:@""];
    self.closePanel = [menu addItemWithTitle:@"Close AT6301" action:@selector(closePanel:)
                               keyEquivalent:@""];
    [menu addItem:[NSMenuItem separatorItem]];
    [menu addItemWithTitle:@"Quit menu bar icon" action:@selector(quit:) keyEquivalent:@""];
    for (NSMenuItem *mi in menu.itemArray) mi.target = self;
    self.item.menu = menu;
}

- (BOOL)panelRunning
{
    return self.panel && self.panel.running;
}

/* the menu reflects whether the panel is open */
- (void)menuNeedsUpdate:(NSMenu *)menu
{
    BOOL up = [self panelRunning];

    self.openReal.title = up ? @"Show AT6301" : @"Open AT6301";
    self.openSim.enabled = !up;
    self.closePanel.enabled = up;
}

- (void)launch:(BOOL)sim
{
    NSError *err = nil;
    NSTask *t;

    if ([self panelRunning]) {
        [[NSRunningApplication runningApplicationWithProcessIdentifier:self.panel.processIdentifier]
            activateWithOptions:NSApplicationActivateAllWindows];
        return;
    }
    t = [[NSTask alloc] init];
    t.executableURL = [NSURL fileURLWithPath:[self.dir stringByAppendingPathComponent:@"at6301"]];
    t.currentDirectoryURL = [NSURL fileURLWithPath:self.dir];
    t.arguments = sim ? @[ @"--sim" ] : @[];
    if (![t launchAndReturnError:&err]) {
        NSAlert *a = [[NSAlert alloc] init];

        a.messageText = @"Could not start the AT6301 panel";
        a.informativeText = err.localizedDescription ?: @"";
        [a runModal];
        return;
    }
    self.panel = t;
    /* bring it forward once its window is up */
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.8 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        if ([self panelRunning])
            [[NSRunningApplication runningApplicationWithProcessIdentifier:t.processIdentifier]
                activateWithOptions:NSApplicationActivateAllWindows];
    });
}

- (void)openReal:(id)s { [self launch:NO]; }
- (void)openSim:(id)s  { [self launch:YES]; }

/* SIGTERM ends the panel's event loop the same way closing its window does,
   so it still hands the supply back to its front panel */
- (void)closePanel:(id)s
{
    if ([self panelRunning]) [self.panel terminate];
}

- (void)quit:(id)s { [NSApp terminate:nil]; }

@end

int main(void)
{
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        Launcher *l = [[Launcher alloc] init];

        app.delegate = l;
        [app setActivationPolicy:NSApplicationActivationPolicyAccessory];
        [app run];
    }
    return 0;
}
