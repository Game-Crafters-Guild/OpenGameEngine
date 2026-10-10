#include "Logger/Logger.h"
#include "Platform/ContextMenu.h"
#include "Platform/Window.h"

#if defined(__APPLE__)

#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#import <Cocoa/Cocoa.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine { class MacContextMenu; }

@interface GE_MacContextMenuTarget : NSObject <NSSearchFieldDelegate, NSMenuDelegate>
@property(nonatomic, assign) GameEngine::MacContextMenu* menu;
+ (instancetype)shared;
- (IBAction)onMenuItem:(id)sender;
- (IBAction)onSearchField:(id)sender;
- (void)controlTextDidChange:(NSNotification*)notification;
- (void)searchFieldDidEndSearching:(NSSearchField*)sender;
- (void)menuDidClose:(NSMenu*)menu;
@end

namespace GameEngine {

class MacContextMenu;
void MacContextMenuNotifyClosed(MacContextMenu* menu);

class MacContextMenu : public INativeContextMenu {
public:
    MacContextMenu() {
        m_Menu = [[NSMenu alloc] initWithTitle:@"ContextMenu"];
        [m_Menu setDelegate:[GE_MacContextMenuTarget shared]];
    }

    void NotifyClosed() {
        if (m_CloseHandler)
            m_CloseHandler();
    }

    ~MacContextMenu() override {
        m_Alive->store(false);
        Clear();
        m_CommandCallback = nullptr;
        m_StateProvider = nullptr;
    }

    void Clear() override {
        if (!m_Menu) return;
        RemoveKeyMonitor();
        while ([m_Menu numberOfItems] > 0) {
            [m_Menu removeItemAtIndex:0];
        }
        m_SubMenus.clear();
        m_SubMenuItems.clear();
        m_CommandItems.clear();
        m_SearchRows.clear();
        m_SearchFields.clear();
        m_SearchResultItems.clear();
        m_SearchCallback = nullptr;
        m_StaticFlags.clear();
        m_NextSubMenuId = 1;
    }

    uint32_t AddSubMenu(uint32_t parentId, const std::string& title) override {
        NSMenu* parent = GetMenuById(parentId);
        if (!parent) return 0;
        NSString* t = [NSString stringWithUTF8String:title.c_str()];
        NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:t action:nil keyEquivalent:@""];
        NSMenu* sub = [[NSMenu alloc] initWithTitle:t];
        [item setSubmenu:sub];
        [parent addItem:item];
        uint32_t id = m_NextSubMenuId++;
        m_SubMenus[id] = sub;
        m_SubMenuItems[id] = item;
        return id;
    }

    void AddItem(uint32_t parentId, const std::string& title,
                 uint32_t commandId, uint32_t ItemFlags) override {
        NSMenu* parent = GetMenuById(parentId);
        if (!parent) return;

        NSString* t = [NSString stringWithUTF8String:title.c_str()];
        NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:t action:@selector(onMenuItem:) keyEquivalent:@""];
        [item setTarget:[GE_MacContextMenuTarget shared]];
        [item setTag:(NSInteger)commandId];

        bool enabled = (ItemFlags & MenuItemFlag_Disabled) == 0;
        [item setEnabled:enabled];
        if (ItemFlags & MenuItemFlag_Checked) {
            [item setState:NSControlStateValueOn];
        } else {
            [item setState:NSControlStateValueOff];
        }

        [parent addItem:item];
        m_CommandItems[commandId].push_back(item);
        if (ItemFlags != MenuItemFlag_None) {
            m_StaticFlags[commandId] = ItemFlags;
        }
    }

    void AddSeparator(uint32_t parentId) override {
        NSMenu* parent = GetMenuById(parentId);
        if (!parent) return;
        [parent addItem:[NSMenuItem separatorItem]];
    }

    bool AddSearchField(uint32_t parentId,
                        const std::string& placeholder,
                        const std::string& initialText,
                        SearchCallback onSearch) override {
        NSMenu* parent = GetMenuById(parentId);
        if (!parent) return false;

        NSView* row = [[NSView alloc] initWithFrame:NSMakeRect(0.0, 0.0, 1.0, 32.0)];
        NSSearchField* field = [[NSSearchField alloc] initWithFrame:NSMakeRect(8.0, 2.0, 1.0, 28.0)];
        NSString* p = [NSString stringWithUTF8String:placeholder.c_str()];
        NSString* text = [NSString stringWithUTF8String:initialText.c_str()];
        [field setPlaceholderString:p ? p : @""];
        [field setStringValue:text ? text : @""];
        [field setFocusRingType:NSFocusRingTypeNone];
        [field setContinuous:YES];
        if ([field respondsToSelector:@selector(setSendsSearchStringImmediately:)]) {
            [field setSendsSearchStringImmediately:YES];
        }
        [field setTarget:[GE_MacContextMenuTarget shared]];
        [field setAction:@selector(onSearchField:)];
        [field setDelegate:[GE_MacContextMenuTarget shared]];
        [row addSubview:field];

        NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:@"" action:nil keyEquivalent:@""];
        [item setView:row];
        [parent addItem:item];

        m_SearchRows.push_back(row);
        m_SearchFields.push_back(field);
        m_SearchCallback = std::move(onSearch);
        [row release];
        return true;
    }

    void SetCommandHandler(CommandCallback cb) override {
        m_CommandCallback = std::move(cb);
        [GE_MacContextMenuTarget shared].menu = this;
    }

    void SetStateProvider(StateProviderCallback cb) override {
        m_StateProvider = std::move(cb);
    }

    void SetItemEnabled(uint32_t commandId, bool enabled) override {
        uint32_t& flags = m_StaticFlags[commandId];
        if (enabled) flags &= ~MenuItemFlag_Disabled;
        else         flags |=  MenuItemFlag_Disabled;
    }

    void SetItemChecked(uint32_t commandId, bool checked) override {
        uint32_t& flags = m_StaticFlags[commandId];
        if (checked) flags |=  MenuItemFlag_Checked;
        else         flags &= ~MenuItemFlag_Checked;
    }

    void SetItemColor(uint32_t commandId, const std::string& hexColor) override {
        if (hexColor.empty()) return;
        NSImage* img = CreateDotImage(hexColor);
        if (!img) return;
        auto it = m_CommandItems.find(commandId);
        if (it != m_CommandItems.end()) {
            for (NSMenuItem* item : it->second)
                [item setImage:img];
        }
        [img release];
    }

    void SetItemIcon(uint32_t commandId, const std::string& imagePath) override {
        if (imagePath.empty()) return;
        NSImage* img = CreateIconImage(imagePath);
        if (!img) return;
        auto it = m_CommandItems.find(commandId);
        if (it != m_CommandItems.end()) {
            for (NSMenuItem* item : it->second)
                [item setImage:img];
        }
        [img release];
    }

    void SetSubMenuIcon(uint32_t submenuId, const std::string& imagePath) override {
        if (imagePath.empty()) return;
        auto it = m_SubMenuItems.find(submenuId);
        if (it == m_SubMenuItems.end()) return;
        NSImage* img = CreateIconImage(imagePath);
        if (!img) return;
        [it->second setImage:img];
        [img release];
    }

    void SetCloseHandler(std::function<void()> cb) override { m_CloseHandler = std::move(cb); }

    void Show(Platform::Window* window, int x, int y) override {
        if (!window || !m_Menu) return;

        GLFWwindow* glfwWin = window->GetGLFWHandle();
        if (!glfwWin) return;

        NSWindow* nsWindow = glfwGetCocoaWindow(glfwWin);
        if (!nsWindow) return;

        NSView* contentView = [nsWindow contentView];
        if (!contentView) return;

        ApplyItemStates();
        ResizeSearchFieldsToRootMenuWidth();

        NSRect bounds = [contentView bounds];
        // Callers pass coordinates in UI-logical pixels (what UIElement::GetLayoutX/Y
        // returns). Cocoa's contentView operates in points. Convert via the ratio
        // UIContentScale / nativeContentScale (the ratio is 1.0 when the UI uses the
        // OS default scale; non-1.0 when an additional HiDPI multiplier is applied).
        float nsx = 1.0f, nsy = 1.0f;
        window->GetContentScale(nsx, nsy);
        const float nativeScale = std::max(0.01f, 0.5f * (nsx + nsy));
        const float uiScale = window->GetUiContentScale();
        const float ratio = (uiScale > 0.01f) ? (uiScale / nativeScale) : 1.0f;
        const CGFloat cocoaX = static_cast<CGFloat>(x) * ratio;
        const CGFloat cocoaY = static_cast<CGFloat>(y) * ratio;
        // Convert from a top-left-like engine coordinate system to Cocoa's default bottom-left origin.
        NSPoint pt = NSMakePoint(cocoaX, bounds.size.height - cocoaY);

        // NSMenu's tracking loop blocks the engine frame that called Show().
        // Defer it by one main-queue turn so click-triggered tooltip dismissal
        // is rendered and presented before Cocoa begins menu tracking.
        NSMenu* popupMenu = [m_Menu retain];
        NSView* popupView = [contentView retain];
        auto alive = m_Alive;
        MacContextMenu* self = this;
        dispatch_async(dispatch_get_main_queue(), ^{
            if (alive->load()) {
                [GE_MacContextMenuTarget shared].menu = self;
                if (!self->m_SearchFields.empty())
                    self->InstallKeyMonitor(self->m_SearchFields.front());
                [popupMenu popUpMenuPositioningItem:nil atLocation:pt inView:popupView];
                self->RemoveKeyMonitor();
                /* Tracking is over by here whether or not an item fired. */
                if (self->m_CloseHandler)
                    self->m_CloseHandler();
            }
            [popupView release];
            [popupMenu release];
        });
    }

    void Dispatch(uint32_t cmd) {
        if (m_CommandCallback) {
            m_CommandCallback(cmd);
        }
    }

    void SearchChanged(NSString* text) {
        ApplySearchFilter(text ? text : @"");
        if (!m_SearchCallback) return;
        const char* utf8 = text ? [text UTF8String] : "";
        m_SearchCallback(utf8 ? std::string(utf8) : std::string());
    }

    void InstallKeyMonitor(NSSearchField* field) {
        RemoveKeyMonitor();
        if (!field) return;

        __block MacContextMenu* menu = this;
        __block NSSearchField* searchField = field;
        m_KeyMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                                             handler:^NSEvent* (NSEvent* event) {
            if (!menu || !searchField)
                return event;

            const NSEventModifierFlags disallowedMods =
                NSEventModifierFlagCommand | NSEventModifierFlagControl | NSEventModifierFlagOption;
            if (([event modifierFlags] & disallowedMods) != 0)
                return event;

            NSString* chars = [event characters];
            if (!chars || [chars length] == 0)
                return event;

            const unichar first = [chars characterAtIndex:0];
            NSMutableString* next = [[searchField stringValue] mutableCopy];
            bool handled = false;

            if (first == NSBackspaceCharacter || first == NSDeleteCharacter) {
                if ([next length] > 0)
                    [next deleteCharactersInRange:NSMakeRange([next length] - 1, 1)];
                handled = true;
            } else if (first == NSEnterCharacter || first == NSCarriageReturnCharacter ||
                       first == 0x1B || first == NSTabCharacter ||
                       first == NSUpArrowFunctionKey || first == NSDownArrowFunctionKey ||
                       first == NSLeftArrowFunctionKey || first == NSRightArrowFunctionKey) {
                handled = false;
            } else {
                [next appendString:chars];
                handled = true;
            }

            if (handled) {
                [searchField setStringValue:next];
                menu->SearchChanged(next);
                [next release];
                return nil;
            }

            [next release];
            return event;
        }];
    }

    void RemoveKeyMonitor() {
        if (m_KeyMonitor) {
            [NSEvent removeMonitor:m_KeyMonitor];
            m_KeyMonitor = nil;
        }
    }

    void ResizeSearchFieldsToRootMenuWidth() {
        if (!m_Menu || m_SearchFields.empty())
            return;

        CGFloat widestTitle = 0.0;
        NSDictionary* attrs = @{ NSFontAttributeName: [NSFont menuFontOfSize:0.0] };
        for (NSMenuItem* item in [m_Menu itemArray]) {
            if (IsSearchFieldItem(item) || [item isSeparatorItem])
                continue;
            NSString* title = [item title] ? [item title] : @"";
            if ([title length] == 0)
                continue;
            widestTitle = std::max(widestTitle, [title sizeWithAttributes:attrs].width);
        }

        // Estimate the native root menu width without letting the custom search
        // view dictate it. The search field then fills that row with 8px margins.
        constexpr CGFloat kMenuChromeWidth = 96.0;
        constexpr CGFloat kMinMenuWidth = 180.0;
        constexpr CGFloat kSearchMargin = 8.0;
        const CGFloat menuWidth = std::max(kMinMenuWidth, widestTitle + kMenuChromeWidth);

        for (NSView* row : m_SearchRows) {
            NSRect rowFrame = [row frame];
            rowFrame.size.width = menuWidth;
            [row setFrame:rowFrame];
        }

        for (NSSearchField* field : m_SearchFields) {
            NSRect fieldFrame = [field frame];
            fieldFrame.origin.x = kSearchMargin;
            fieldFrame.size.width = std::max<CGFloat>(1.0, menuWidth - (kSearchMargin * 2.0));
            [field setFrame:fieldFrame];
        }
    }

    static NSImage* CreateDotImage(const std::string& hexColor) {
        if (hexColor.empty() || (hexColor[0] != '#')) return nil;
        double r = 0, g = 0, b = 0;
        if (hexColor.size() == 7) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return 0;
            };
            r = (hex(hexColor[1]) * 16 + hex(hexColor[2])) / 255.0;
            g = (hex(hexColor[3]) * 16 + hex(hexColor[4])) / 255.0;
            b = (hex(hexColor[5]) * 16 + hex(hexColor[6])) / 255.0;
        } else if (hexColor.size() == 4) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return 0;
            };
            r = (hex(hexColor[1]) * 17) / 255.0;
            g = (hex(hexColor[2]) * 17) / 255.0;
            b = (hex(hexColor[3]) * 17) / 255.0;
        } else {
            return nil;
        }
        NSColor* color = [NSColor colorWithRed:r green:g blue:b alpha:1.0];
        const CGFloat size = 10.0;
        NSImage* img = [[NSImage alloc] initWithSize:NSMakeSize(size, size)];
        [img lockFocus];
        [color set];
        [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(0, 0, size, size)] fill];
        [img unlockFocus];
        return img;
    }

    static NSImage* CreateIconImage(const std::string& imagePath) {
        NSString* path = ResolveIconPath(imagePath);
        if (!path) return nil;

        NSImage* img = [[NSImage alloc] initWithContentsOfFile:path];
        if (!img) return nil;

        [img setSize:NSMakeSize(16.0, 16.0)];
        return img;
    }

    static NSString* ResolveIconPath(const std::string& imagePath) {
        if (imagePath.empty()) return nil;

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

private:
    NSMenu* GetMenuById(uint32_t id) const {
        if (id == 0) return m_Menu;
        auto it = m_SubMenus.find(id);
        return (it == m_SubMenus.end()) ? nil : it->second;
    }

    bool MenuItemMatchesSearch(NSMenuItem* item, NSString* query) {
        if (!item) return false;
        if (IsSearchFieldItem(item))
            return true;
        if ([item isSeparatorItem])
            return [query length] == 0;

        const bool hasQuery = [query length] > 0;
        NSString* title = [item title] ? [item title] : @"";
        const bool directMatch =
            !hasQuery ||
            [title rangeOfString:query options:NSCaseInsensitiveSearch | NSDiacriticInsensitiveSearch].location != NSNotFound;
        bool descendantMatch = false;

        NSMenu* sub = [item submenu];
        if (sub) {
            for (NSMenuItem* child in [sub itemArray]) {
                const bool childMatches = MenuItemMatchesSearch(child, query);
                [child setHidden:!childMatches];
                descendantMatch = descendantMatch || childMatches;
            }
        }

        const bool matches = directMatch || descendantMatch;
        ApplySearchHighlight(item, hasQuery, directMatch, descendantMatch);
        return matches;
    }

    void CollectLeafSearchResults(NSMenuItem* item,
                                  NSString* query,
                                  NSString* pathPrefix,
                                  NSMutableArray<NSMenuItem*>* outResults) {
        if (!item || !outResults)
            return;
        if ([item view] && [[item view] isKindOfClass:[NSSearchField class]])
            return;
        if ([item isSeparatorItem])
            return;

        NSString* title = [item title] ? [item title] : @"";
        NSString* path = [pathPrefix length] > 0
            ? [NSString stringWithFormat:@"%@ > %@", pathPrefix, title]
            : title;

        NSMenu* sub = [item submenu];
        if (sub) {
            for (NSMenuItem* child in [sub itemArray])
                CollectLeafSearchResults(child, query, path, outResults);
            return;
        }

        if ([title length] == 0)
            return;

        const bool titleMatches =
            [title rangeOfString:query options:NSCaseInsensitiveSearch | NSDiacriticInsensitiveSearch].location != NSNotFound;
        const bool pathMatches =
            [path rangeOfString:query options:NSCaseInsensitiveSearch | NSDiacriticInsensitiveSearch].location != NSNotFound;
        if (!titleMatches && !pathMatches)
            return;

        NSMenuItem* result = [[NSMenuItem alloc] initWithTitle:title
                                                        action:@selector(onMenuItem:)
                                                 keyEquivalent:@""];
        [result setTarget:[GE_MacContextMenuTarget shared]];
        [result setTag:[item tag]];
        [result setEnabled:[item isEnabled]];
        [result setState:[item state]];
        [result setImage:[item image]];
        [result setToolTip:path];
        ApplySearchHighlight(result, true, true, false);
        [outResults addObject:result];
        [result release];
    }

    void ApplySearchHighlight(NSMenuItem* item, bool hasQuery, bool directMatch, bool descendantMatch) {
        if (!item) return;
        if (!hasQuery || (!directMatch && !descendantMatch)) {
            [item setAttributedTitle:nil];
            return;
        }

        NSString* title = [item title] ? [item title] : @"";
        if ([title length] == 0)
            return;

        NSMutableAttributedString* styled =
            [[NSMutableAttributedString alloc] initWithString:title];
        if (directMatch) {
            NSFont* font = [NSFont boldSystemFontOfSize:[NSFont systemFontSize]];
            [styled addAttribute:NSFontAttributeName value:font range:NSMakeRange(0, [title length])];
        }

        [item setAttributedTitle:styled];
        [styled release];
    }

    void ApplySearchFilter(NSString* query) {
        if (!m_Menu) return;
        RemoveSearchResults();
        const bool hasQuery = query && [query length] > 0;

        for (NSMenuItem* item in [m_Menu itemArray]) {
            if (IsSearchResultItem(item))
                continue;

            if (IsSearchFieldItem(item)) {
                [item setHidden:NO];
                continue;
            }

            if (!hasQuery) {
                [item setHidden:NO];
                [item setAttributedTitle:nil];
                RestoreSubmenuItems(item);
                continue;
            }

            (void)MenuItemMatchesSearch(item, query);
            [item setHidden:YES];
        }

        if (!hasQuery)
            return;

        NSMutableArray<NSMenuItem*>* results = [NSMutableArray array];
        for (NSMenuItem* item in [m_Menu itemArray]) {
            if (IsSearchResultItem(item))
                continue;
            CollectLeafSearchResults(item, query, @"", results);
        }

        for (NSMenuItem* item in results) {
            [m_Menu addItem:item];
            m_SearchResultItems.push_back(item);
        }
        ResizeSearchFieldsToRootMenuWidth();
    }

    bool IsSearchResultItem(NSMenuItem* item) const {
        return std::find(m_SearchResultItems.begin(), m_SearchResultItems.end(), item) != m_SearchResultItems.end();
    }

    bool IsSearchFieldItem(NSMenuItem* item) const {
        if (!item)
            return false;
        NSView* view = [item view];
        if (!view)
            return false;
        if ([view isKindOfClass:[NSSearchField class]])
            return true;
        for (NSView* child in [view subviews]) {
            if ([child isKindOfClass:[NSSearchField class]])
                return true;
        }
        return false;
    }

    void RemoveSearchResults() {
        if (!m_Menu)
            return;
        for (NSMenuItem* item : m_SearchResultItems) {
            if ([item menu] == m_Menu)
                [m_Menu removeItem:item];
        }
        m_SearchResultItems.clear();
    }

    void RestoreSubmenuItems(NSMenuItem* item) {
        if (!item)
            return;
        NSMenu* sub = [item submenu];
        if (!sub)
            return;
        for (NSMenuItem* child in [sub itemArray]) {
            [child setHidden:NO];
            [child setAttributedTitle:nil];
            RestoreSubmenuItems(child);
        }
    }

    void ApplyItemStates() {
        for (auto& pair : m_CommandItems) {
            uint32_t cmd = pair.first;

            uint32_t baseFlags = 0;
            auto itFlags = m_StaticFlags.find(cmd);
            if (itFlags != m_StaticFlags.end()) baseFlags = itFlags->second;

            bool enabled = (baseFlags & MenuItemFlag_Disabled) == 0;
            bool checked = (baseFlags & MenuItemFlag_Checked) != 0;

            if (m_StateProvider) {
                MenuItemState state = m_StateProvider(cmd);
                enabled = enabled && state.Enabled;
                checked = state.Checked;
            }

            for (NSMenuItem* item : pair.second) {
                [item setEnabled:enabled];
                [item setState:(checked ? NSControlStateValueOn : NSControlStateValueOff)];
            }
        }
    }

    NSMenu* m_Menu = nil;
    std::unordered_map<uint32_t, NSMenu*> m_SubMenus;
    std::unordered_map<uint32_t, NSMenuItem*> m_SubMenuItems;
    std::unordered_map<uint32_t, std::vector<NSMenuItem*>> m_CommandItems;
    std::vector<NSView*> m_SearchRows;
    std::vector<NSSearchField*> m_SearchFields;
    std::vector<NSMenuItem*> m_SearchResultItems;
    std::shared_ptr<std::atomic<bool>> m_Alive = std::make_shared<std::atomic<bool>>(true);
    std::unordered_map<uint32_t, uint32_t> m_StaticFlags;
    uint32_t m_NextSubMenuId = 1;
    CommandCallback m_CommandCallback;
    std::function<void()> m_CloseHandler;

    friend void MacContextMenuNotifyClosed(MacContextMenu* menu);
    StateProviderCallback m_StateProvider;
    SearchCallback m_SearchCallback;
    id m_KeyMonitor = nil;
};

std::unique_ptr<INativeContextMenu> CreateNativeContextMenu() {
    return std::make_unique<MacContextMenu>();
}

void MacContextMenuNotifyClosed(MacContextMenu* menu)
{
    if (menu)
        menu->NotifyClosed();
}

} // namespace GameEngine

@implementation GE_MacContextMenuTarget

+ (instancetype)shared {
    static GE_MacContextMenuTarget* sInst = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{ sInst = [GE_MacContextMenuTarget new]; });
    return sInst;
}

- (IBAction)onMenuItem:(id)sender {
    NSInteger tag = [sender tag];
    if (self.menu) { self.menu->Dispatch((uint32_t)tag); }
}

- (IBAction)onSearchField:(id)sender {
    if (!self.menu) { return; }
    if (![sender respondsToSelector:@selector(stringValue)]) { return; }
    self.menu->SearchChanged([sender stringValue]);
}

- (void)controlTextDidChange:(NSNotification*)notification {
    if (!self.menu) { return; }
    id sender = [notification object];
    if (![sender respondsToSelector:@selector(stringValue)]) { return; }
    self.menu->SearchChanged([sender stringValue]);
}

- (void)searchFieldDidEndSearching:(NSSearchField*)sender {
    if (!self.menu || !sender) { return; }
    self.menu->SearchChanged([sender stringValue]);
}

/* Cocoa reports dismissal here whatever caused it — a click outside the menu,
   Escape, or an item firing — which is earlier and more reliable than watching
   popUpMenuPositioningItem return. */
- (void)menuDidClose:(NSMenu*)menu {
    (void)menu;
    if (self.menu)
        GameEngine::MacContextMenuNotifyClosed(self.menu);
}

@end

#endif // __APPLE__
