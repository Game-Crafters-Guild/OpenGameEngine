#include "Platform/Toolbar.h"
#include "Platform/Window.h"

#if defined(__APPLE__)
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#import <Cocoa/Cocoa.h>
#include <unordered_map>
#include <memory>
#include <string>

namespace GameEngine { class MacToolbar; }

@interface GE_MacToolbarTarget : NSObject
@property(nonatomic, assign) GameEngine::MacToolbar* toolbar;
+ (instancetype)shared;
- (IBAction)onMenuItem:(id)sender;
@end

namespace GameEngine {

class MacToolbar : public INativeToolbar {
public:
    bool Install(Platform::Window* /*window*/) override {
        if (!m_MenuBar) {
            m_MenuBar = [[NSMenu alloc] initWithTitle:@"MainMenu"];
            [NSApp setMainMenu:m_MenuBar];
            InsertAppMenuPlaceholder();
        } else {
            Clear();
        }
        // Ensure target routes to this instance
        [GE_MacToolbarTarget shared].toolbar = this;
        return true;
    }

    void Uninstall() override {
        // On macOS, the menu bar is app-global. We'll just clear our items.
        Clear();
        m_Callback = nullptr;
    }

    void Clear() override {
        if (!m_MenuBar) return;
        while ([m_MenuBar numberOfItems] > 0) {
            [m_MenuBar removeItemAtIndex:0];
        }
        m_Menus.clear();
        m_NextMenuId = 1;
        m_Items.clear();
        InsertAppMenuPlaceholder();
    }

    // macOS treats the first menu item as the application menu (label = app name).
    // Populate it with the standard Cocoa items (About, Services, Hide, Quit) so
    // subsequent AddMenu("Edit") calls land as real "Edit" entries and the user
    // keeps Cmd-Q, Hide, etc. that GLFW's default menu bar would otherwise have.
    void InsertAppMenuPlaceholder() {
        if (!m_MenuBar) return;

        NSString* appName = [[NSProcessInfo processInfo] processName];

        NSMenuItem* appItem = [[NSMenuItem alloc] initWithTitle:@"" action:nil keyEquivalent:@""];
        NSMenu* appMenu = [[NSMenu alloc] initWithTitle:@""];

        NSString* aboutTitle = [@"About " stringByAppendingString:appName];
        [appMenu addItemWithTitle:aboutTitle
                           action:@selector(orderFrontStandardAboutPanel:)
                    keyEquivalent:@""];
        [appMenu addItem:[NSMenuItem separatorItem]];

        NSMenuItem* servicesItem = [[NSMenuItem alloc] initWithTitle:@"Services"
                                                              action:nil
                                                       keyEquivalent:@""];
        NSMenu* servicesMenu = [[NSMenu alloc] initWithTitle:@"Services"];
        [servicesItem setSubmenu:servicesMenu];
        [appMenu addItem:servicesItem];
        [NSApp setServicesMenu:servicesMenu];
        [appMenu addItem:[NSMenuItem separatorItem]];

        NSString* hideTitle = [@"Hide " stringByAppendingString:appName];
        [appMenu addItemWithTitle:hideTitle
                           action:@selector(hide:)
                    keyEquivalent:@"h"];

        NSMenuItem* hideOthersItem = [appMenu addItemWithTitle:@"Hide Others"
                                                        action:@selector(hideOtherApplications:)
                                                 keyEquivalent:@"h"];
        [hideOthersItem setKeyEquivalentModifierMask:NSEventModifierFlagOption | NSEventModifierFlagCommand];

        [appMenu addItemWithTitle:@"Show All"
                           action:@selector(unhideAllApplications:)
                    keyEquivalent:@""];
        [appMenu addItem:[NSMenuItem separatorItem]];

        NSString* quitTitle = [@"Quit " stringByAppendingString:appName];
        [appMenu addItemWithTitle:quitTitle
                           action:@selector(terminate:)
                    keyEquivalent:@"q"];

        [appItem setSubmenu:appMenu];
        [m_MenuBar addItem:appItem];
    }

    void SetCommandHandler(CommandCallback cb) override {
        m_Callback = std::move(cb);
        [GE_MacToolbarTarget shared].toolbar = this;
    }

    uint32_t AddMenu(const std::string& title) override {
        if (!m_MenuBar) return 0;
        NSString* t = [NSString stringWithUTF8String:title.c_str()];
        NSMenuItem* topItem = [[NSMenuItem alloc] initWithTitle:t action:nil keyEquivalent:@""];
        NSMenu* sub = [[NSMenu alloc] initWithTitle:t];
        [topItem setSubmenu:sub];
        [m_MenuBar addItem:topItem];
        uint32_t id = m_NextMenuId++;
        m_Menus[id] = sub;
        return id;
    }

    uint32_t AddSubMenu(uint32_t parentMenuId, const std::string& title) override {
        NSMenu* parent = GetMenuById(parentMenuId);
        if (!parent) return 0;
        NSString* t = [NSString stringWithUTF8String:title.c_str()];
        NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:t action:nil keyEquivalent:@""];
        NSMenu* sub = [[NSMenu alloc] initWithTitle:t];
        [item setSubmenu:sub];
        [parent addItem:item];
        uint32_t id = m_NextMenuId++;
        m_Menus[id] = sub;
        return id;
    }

    void AddItem(uint32_t parentMenuId, const std::string& title, uint32_t commandId) override {
        NSMenu* parent = GetMenuById(parentMenuId);
        if (!parent) return;
        NSString* t = [NSString stringWithUTF8String:title.c_str()];
        NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:t action:@selector(onMenuItem:) keyEquivalent:@""];
        [item setTarget:[GE_MacToolbarTarget shared]];
        [item setTag:(NSInteger)commandId];
        [parent addItem:item];
	        m_Items[commandId] = item;
    }

    void SetItemIcon(uint32_t commandId, const std::string& imagePath) override {
        auto it = m_Items.find(commandId);
        if (it == m_Items.end() || !it->second || imagePath.empty()) return;

        NSString* path = ResolveIconPath(imagePath);
        if (!path) return;
        NSImage* image = [[NSImage alloc] initWithContentsOfFile:path];
        if (!image) return;
        [image setSize:NSMakeSize(16.0, 16.0)];
        [it->second setImage:image];
        [image release];
    }

    void UpdateItemTitle(uint32_t commandId, const std::string& title) override {
        auto it = m_Items.find(commandId);
        if (it == m_Items.end()) return;
        NSMenuItem* item = it->second;
        if (!item) return;
        NSString* t = [NSString stringWithUTF8String:title.c_str()];
        [item setTitle:t];
    }

    void Dispatch(uint32_t cmd) { if (m_Callback) m_Callback(cmd); }

private:
    static NSString* ResolveIconPath(const std::string& imagePath) {
        std::string path = imagePath;
        bool editorAlias = false;
        constexpr const char* kEditorColon = "editor:";
        constexpr const char* kEditorSlash = "@editor/";
        if (path.rfind(kEditorColon, 0) == 0) {
            path = path.substr(std::char_traits<char>::length(kEditorColon));
            editorAlias = true;
        } else if (path.rfind(kEditorSlash, 0) == 0) {
            path = path.substr(std::char_traits<char>::length(kEditorSlash));
            editorAlias = true;
        }

        NSString* nsPath = [NSString stringWithUTF8String:path.c_str()];
        if (!nsPath) return nil;
        if ([nsPath isAbsolutePath]) return [nsPath stringByStandardizingPath];

        NSString* base = [[NSBundle mainBundle] resourcePath];
        if (!base) base = [[NSBundle mainBundle] bundlePath];
        if (!base) return nil;
        if (editorAlias)
            base = [base stringByAppendingPathComponent:@"Assets"];
        return [[base stringByAppendingPathComponent:nsPath] stringByStandardizingPath];
    }

    NSMenu* GetMenuById(uint32_t id) const {
        auto it = m_Menus.find(id);
        return (it == m_Menus.end()) ? nil : it->second;
    }

    NSMenu* m_MenuBar = nil;
	    std::unordered_map<uint32_t, NSMenu*> m_Menus;
	    std::unordered_map<uint32_t, NSMenuItem*> m_Items;
    uint32_t m_NextMenuId = 1;
    CommandCallback m_Callback;
};

std::unique_ptr<INativeToolbar> CreateNativeToolbar() {
    return std::make_unique<MacToolbar>();
}

} // namespace GameEngine

@implementation GE_MacToolbarTarget

+ (instancetype)shared {
    static GE_MacToolbarTarget* sInst = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{ sInst = [GE_MacToolbarTarget new]; });
    return sInst;
}

- (IBAction)onMenuItem:(id)sender {
    NSInteger tag = [sender tag];
    if (self.toolbar) { self.toolbar->Dispatch((uint32_t)tag); }
}

@end

#endif // __APPLE__
