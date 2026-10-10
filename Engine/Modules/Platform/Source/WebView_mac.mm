#if defined(__APPLE__)

#include "Platform/WebView.h"
#include "Platform/Window.h"

#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#import <QuartzCore/QuartzCore.h>

// Private API to enable HTML5 fullscreen in WKWebView (YouTube and other sites).
// Without this, pages show "Your browser doesn't support fullscreen".
@interface WKPreferences (Fullscreen)
- (void)_setFullScreenEnabled:(BOOL)enabled;
@end

// Custom navigation delegate to intercept YouTube embed requests and inject proper headers
@interface YouTubeNavigationDelegate : NSObject <WKNavigationDelegate>
@end

@interface WebViewFileDragSource : NSObject <NSDraggingSource>
@end

// Dark background injected into every loaded page so a light page never flashes white
// before it paints. Runs at document-start, where document.body and document.head are
// still null, and again after navigation for pages that rebuild <head> during load.
//
// The page scrollbar is deliberately left alone: macOS WebKit draws it as a native
// scroller, which ignores ::-webkit-scrollbar and scrollbar-color (scrollbar-width is
// the only property that takes effect), so it cannot be themed to match the editor.
static NSString* const kPageStyleScript = @""
    "(function() {"
    "  var root = document.documentElement;"
    "  if (root) {"
    "    root.style.backgroundColor = '#272727';"
    "  }"
    "  if (document.body) {"
    "    document.body.style.backgroundColor = '#272727';"
    "  }"
    "  var style = document.getElementById('ge-injected-style');"
    "  if (style) {"
    "    if (document.head) {"
    "      document.head.appendChild(style);"  // move last so it outranks page styles
    "    }"
    "    return;"
    "  }"
    "  style = document.createElement('style');"
    "  style.id = 'ge-injected-style';"
    "  style.textContent = 'html, body { background-color: #272727 !important; }';"
    "  var attach = function() {"
    "    if (document.head) {"
    "      document.head.appendChild(style);"
    "    }"
    "  };"
    "  if (document.head) {"
    "    attach();"
    "  } else {"
    "    document.addEventListener('DOMContentLoaded', attach);"
    "  }"
    "})();";

@implementation YouTubeNavigationDelegate

- (void)webView:(WKWebView *)webView decidePolicyForNavigationAction:(WKNavigationAction *)navigationAction decisionHandler:(void (^)(WKNavigationActionPolicy))decisionHandler {
    // Allow all navigation - the base URL and referrerpolicy should handle referrer
    // WKWebView should send Referer header based on the base URL of the parent page
    decisionHandler(WKNavigationActionPolicyAllow);
}

- (void)webView:(WKWebView *)webView didStartProvisionalNavigation:(WKNavigation *)navigation {
    NSView* view = (NSView*)webView;
    if (view && view.layer) {
        view.layer.backgroundColor = [[NSColor colorWithRed:39.0/255.0 green:39.0/255.0 blue:39.0/255.0 alpha:1.0] CGColor];
    }
}

- (void)webView:(WKWebView *)webView didFinishNavigation:(WKNavigation *)navigation {
    NSView* view = (NSView*)webView;
    if (view && view.layer) {
        view.layer.backgroundColor = [[NSColor colorWithRed:39.0/255.0 green:39.0/255.0 blue:39.0/255.0 alpha:1.0] CGColor];
    }
    
    // Inject JavaScript to ensure dark background persists and check for YouTube errors
    // Re-assert after navigation: some pages replace <head> during load.
    NSString* script = [kPageStyleScript stringByAppendingString:@""
        "(function() {"
        "  var iframes = document.querySelectorAll('iframe');"
        "  for (var i = 0; i < iframes.length; i++) {"
        "    iframes[i].style.backgroundColor = '#272727';"
        "  }"
        "})();"];
    
    [webView evaluateJavaScript:script completionHandler:^(id result, NSError* error) {
        if (error) {
            NSLog(@"JavaScript injection error: %@", error);
        }
        // Webview visibility is managed by the panel, don't force show here
    }];
}

- (void)webView:(WKWebView *)webView didFailNavigation:(WKNavigation *)navigation withError:(NSError *)error {
    NSLog(@"Navigation failed: %@", error);
}

- (void)webView:(WKWebView *)webView didFailProvisionalNavigation:(WKNavigation *)navigation withError:(NSError *)error {
    NSLog(@"Provisional navigation failed: %@", error);
}

@end

@implementation WebViewFileDragSource

- (NSDragOperation)draggingSession:(NSDraggingSession*)session sourceOperationMaskForDraggingContext:(NSDraggingContext)context
{
    (void)session;
    (void)context;
    return NSDragOperationCopy;
}

- (BOOL)ignoreModifierKeysForDraggingSession:(NSDraggingSession*)session
{
    (void)session;
    return YES;
}

@end

namespace GameEngine {
namespace Platform {

class WebView_mac : public IWebView {
public:
    WebView_mac(Window* parentWindow) : m_ParentWindow(parentWindow) {
        if (!parentWindow) {
            return;
        }

        GLFWwindow* glfwWindow = parentWindow->GetGLFWHandle();
        if (!glfwWindow) {
            return;
        }

        // Get NSWindow from GLFW window
        NSWindow* window = glfwGetCocoaWindow(glfwWindow);
        if (!window) {
            return;
        }
        NSView* contentView = [window contentView];
        if (!contentView) {
            return;
        }
        m_HostView = contentView;

        // Create WKWebView with configuration for YouTube embeds
        WKWebViewConfiguration* config = [[WKWebViewConfiguration alloc] init];
        config.allowsAirPlayForMediaPlayback = YES;
        config.mediaTypesRequiringUserActionForPlayback = WKAudiovisualMediaTypeNone;
        
        // Enable JavaScript (required for YouTube embeds)
        WKPreferences* preferences = [[WKPreferences alloc] init];
        preferences.javaScriptEnabled = YES;
        // Enable HTML5 fullscreen API so YouTube (and other sites) can go fullscreen
        if ([preferences respondsToSelector:@selector(_setFullScreenEnabled:)]) {
            [preferences _setFullScreenEnabled:YES];
        }
        config.preferences = preferences;
        
        // Set process pool to allow proper referrer handling
        if (!config.processPool) {
            config.processPool = [[WKProcessPool alloc] init];
        }
        
        // Configure website data store for proper cookie/security settings
        // Use default data store which supports cookies and secure connections
        config.websiteDataStore = [WKWebsiteDataStore defaultDataStore];
        
        // Set application name for user agent to appear as Safari
        if (@available(macOS 10.13, *)) {
            config.applicationNameForUserAgent = @"Version/17.0 Safari/605.1.15";
        }
        
        // Enable additional features for better compatibility
        if (@available(macOS 11.3, *)) {
            // Allow media playback without user gesture
            config.mediaTypesRequiringUserActionForPlayback = WKAudiovisualMediaTypeNone;
        }
        
        // Create user content controller to inject referrer policy
        WKUserContentController* userContentController = [[WKUserContentController alloc] init];
        
        // Inject referrer policy and dark background script at document start
        NSString* injectScript = [kPageStyleScript stringByAppendingString:@""
            "(function() {"
            "  if (document.querySelector('meta[name=\"referrer\"]') !== null) {"
            "    return;"
            "  }"
            "  var meta = document.createElement('meta');"
            "  meta.name = 'referrer';"
            "  meta.content = 'strict-origin-when-cross-origin';"
            "  var attachMeta = function() {"
            "    if (document.head) {"
            "      document.head.appendChild(meta);"
            "    }"
            "  };"
            "  if (document.head) {"
            "    attachMeta();"
            "  } else {"
            "    document.addEventListener('DOMContentLoaded', attachMeta);"
            "  }"
            "})();"];
        
        WKUserScript* userScript = [[WKUserScript alloc] initWithSource:injectScript
                                                           injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                                        forMainFrameOnly:NO]; // Inject in all frames
        [userContentController addUserScript:userScript];
        config.userContentController = userContentController;

        m_WebView = [[WKWebView alloc] initWithFrame:NSZeroRect configuration:config];
        
        // Enable layer-backed view for proper background color rendering
        NSView* webViewAsView = (NSView*)m_WebView;
        webViewAsView.wantsLayer = YES;
        if (webViewAsView.layer) {
            webViewAsView.layer.backgroundColor = [[NSColor colorWithRed:39.0/255.0 green:39.0/255.0 blue:39.0/255.0 alpha:1.0] CGColor];
        }
        
        // Set user agent to match latest Safari to avoid "browser not secure" errors
        // Use a modern Safari user agent string that YouTube recognizes as secure
        NSString* safariUserAgent = @"Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.0 Safari/605.1.15";
        [m_WebView setCustomUserAgent:safariUserAgent];
        
        // Configure for better YouTube compatibility and security
        [m_WebView setAllowsLinkPreview:NO];
        [m_WebView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
        [m_WebView setHidden:YES]; // Start hidden until positioned and content loaded
        [m_WebView setAllowsBackForwardNavigationGestures:NO];
        
        // Make the web content area non-opaque so the dark layer background
        // shows through until the page actually renders (prevents white flash).
        [m_WebView setValue:@NO forKey:@"drawsBackground"];

        if (@available(macOS 12.0, *)) {
            NSColor* underPage = [NSColor colorWithRed:39.0/255.0 green:39.0/255.0 blue:39.0/255.0 alpha:1.0];
            [m_WebView setUnderPageBackgroundColor:underPage];
        }
        
        // Enable media playback features
        if (@available(macOS 10.12.2, *)) {
            [m_WebView setAllowsMagnification:NO];
        }
        
        // Set navigation delegate to intercept YouTube embed requests
        m_NavigationDelegate = [[YouTubeNavigationDelegate alloc] init];
        [m_WebView setNavigationDelegate:m_NavigationDelegate];

        // Add to content view. Clip to bounds so the web view never draws outside the YouTube panel area.
        [contentView addSubview:m_WebView];
        [webViewAsView setClipsToBounds:YES];

        // WKWebView swallows mouse events before the engine's input path sees them,
        // so watch for clicks landing inside the view and forward a notification.
        WKWebView* webViewRef = m_WebView;
        std::function<void()>* callbackRef = &m_OnMouseDown;
        m_MouseDownMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:(NSEventMaskLeftMouseDown | NSEventMaskRightMouseDown | NSEventMaskOtherMouseDown)
                                                                   handler:^NSEvent*(NSEvent* event) {
            NSView* view = (NSView*)webViewRef;
            if (*callbackRef && view && ![view isHidden] && event.window == [view window]) {
                NSPoint pointInView = [view convertPoint:[event locationInWindow] fromView:nil];
                if (NSPointInRect(pointInView, [view bounds])) {
                    (*callbackRef)();
                }
            }
            return event;
        }];

        m_Initialized = true;
    }

    ~WebView_mac() override {
        Destroy();
    }

    bool LoadURL(const std::string& url) override {
        if (!m_Initialized || !m_WebView) {
            return false;
        }

        NSView* webViewAsView = (NSView*)m_WebView;
        if (webViewAsView && webViewAsView.layer) {
            webViewAsView.layer.backgroundColor = [[NSColor colorWithRed:39.0/255.0 green:39.0/255.0 blue:39.0/255.0 alpha:1.0] CGColor];
        }

        NSString* nsURL = [NSString stringWithUTF8String:url.c_str()];
        NSURL* requestURL = [NSURL URLWithString:nsURL];
        if (!requestURL) {
            return false;
        }

        NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:requestURL];

        // Only set YouTube-specific Referer/Origin for YouTube URLs (required to prevent error 153/152-4).
        // Other sites may reject or misbehave if given a YouTube referer.
        if (url.find("youtube.com") != std::string::npos || url.find("youtu.be") != std::string::npos) {
            [request setValue:@"https://www.youtube.com" forHTTPHeaderField:@"Referer"];
            [request setValue:@"https://www.youtube.com" forHTTPHeaderField:@"Origin"];
        }
        if (@available(macOS 13.0, *)) {
            [request setValue:@"strict-origin-when-cross-origin" forHTTPHeaderField:@"Referrer-Policy"];
        }
        [request setCachePolicy:NSURLRequestReloadIgnoringLocalCacheData];
        [request setTimeoutInterval:30.0];

        [m_WebView loadRequest:request];
        return true;
    }

    bool LoadHTML(const std::string& html, const std::string& baseURL) override {
        if (!m_Initialized || !m_WebView) {
            return false;
        }

        NSString* htmlString = [NSString stringWithUTF8String:html.c_str()];
        NSString* baseURLString = [NSString stringWithUTF8String:baseURL.c_str()];
        NSURL* baseNSURL = [NSURL URLWithString:baseURLString];
        
        if (!baseNSURL) {
            baseNSURL = [NSURL URLWithString:@"https://www.youtube.com"];
        }
        
        [m_WebView loadHTMLString:htmlString baseURL:baseNSURL];
        return true;
    }

    void* GetNativeView() const override {
        return (__bridge void*)m_WebView;
    }

    void Resize(int width, int height) override {
        if (!m_Initialized || !m_WebView) {
            return;
        }

        NSRect frame = [m_WebView frame];
        frame.size.width = static_cast<CGFloat>(width);
        frame.size.height = static_cast<CGFloat>(height);
        [m_WebView setFrame:frame];
    }

    void SetPosition(int x, int y) override {
        if (!m_Initialized || !m_WebView) {
            return;
        }

        NSRect frame = [m_WebView frame];
        // Convert from top-left to bottom-left coordinate system
        GLFWwindow* glfwWindow = m_ParentWindow->GetGLFWHandle();
        if (glfwWindow) {
            NSWindow* window = glfwGetCocoaWindow(glfwWindow);
            if (window) {
                NSView* contentView = [window contentView];
                if (contentView) {
                    NSRect contentRect = [contentView bounds];
                    frame.origin.x = static_cast<CGFloat>(x);
                    frame.origin.y = contentRect.size.height - static_cast<CGFloat>(y) - frame.size.height;
                    [m_WebView setFrame:frame];
                }
            }
        }
    }

    void SetVisible(bool visible) override {
        if (!m_Initialized || !m_WebView) {
            return;
        }
        [m_WebView setHidden:!visible];
    }

    bool IsVisible() const override {
        if (!m_Initialized || !m_WebView) {
            return false;
        }
        return ![m_WebView isHidden];
    }

    void SetBackgroundColor(unsigned char r, unsigned char g, unsigned char b) override {
        if (!m_Initialized || !m_WebView) {
            return;
        }
        // Set WKWebView background color on the underlying NSView's layer
        CGFloat red = static_cast<CGFloat>(r) / 255.0;
        CGFloat green = static_cast<CGFloat>(g) / 255.0;
        CGFloat blue = static_cast<CGFloat>(b) / 255.0;
        NSColor* bgColor = [NSColor colorWithRed:red green:green blue:blue alpha:1.0];
        
        NSView* view = (NSView*)m_WebView;
        if (view && view.layer) {
            view.layer.backgroundColor = [bgColor CGColor];
        }
        
        // Also try setting it via KVC (some versions of WKWebView support this)
        @try {
            [m_WebView setValue:bgColor forKey:@"backgroundColor"];
        } @catch (NSException* exception) {
            // Ignore if property doesn't exist
        }
    }

    bool BeginFileDrag(const std::filesystem::path& path) override {
        if (!m_Initialized || !m_WebView || !m_HostView || path.empty()) {
            return false;
        }

        const std::string utf8Path = path.string();
        NSString* nsPath = [NSString stringWithUTF8String:utf8Path.c_str()];
        if (!nsPath || nsPath.length == 0) {
            return false;
        }

        BOOL isDirectory = NO;
        if (![[NSFileManager defaultManager] fileExistsAtPath:nsPath isDirectory:&isDirectory] || isDirectory) {
            return false;
        }

        NSEvent* event = [NSApp currentEvent];
        if (!event) {
            return false;
        }

        if (([NSEvent pressedMouseButtons] & 1u) == 0u) {
            return false;
        }

        NSEventType eventType = [event type];
        if (eventType != NSEventTypeLeftMouseDragged &&
            eventType != NSEventTypeLeftMouseDown &&
            eventType != NSEventTypeMouseMoved &&
            eventType != NSEventTypeOtherMouseDragged) {
            return false;
        }

        NSURL* fileURL = [NSURL fileURLWithPath:nsPath isDirectory:NO];
        if (!fileURL) {
            return false;
        }

        NSDraggingItem* draggingItem = [[NSDraggingItem alloc] initWithPasteboardWriter:fileURL];
        NSImage* dragImage = [[NSWorkspace sharedWorkspace] iconForFile:nsPath];
        if (dragImage) {
            [dragImage setSize:NSMakeSize(48.0, 48.0)];
        }

        NSPoint locationInView = [m_HostView convertPoint:[event locationInWindow] fromView:nil];
        NSRect dragRect = NSMakeRect(locationInView.x - 24.0, locationInView.y - 24.0, 48.0, 48.0);
        [draggingItem setDraggingFrame:dragRect contents:dragImage];

        if (!m_DragSource) {
            m_DragSource = [[WebViewFileDragSource alloc] init];
        }

        NSDraggingSession* session = [m_HostView beginDraggingSessionWithItems:@[draggingItem]
                                                                         event:event
                                                                        source:m_DragSource];
        if (!session) {
            return false;
        }

        [session setAnimatesToStartingPositionsOnCancelOrFail:NO];
        return true;
    }

    void SetOnMouseDown(std::function<void()> callback) override {
        m_OnMouseDown = std::move(callback);
    }

    void Destroy() override {
        if (m_MouseDownMonitor) {
            [NSEvent removeMonitor:m_MouseDownMonitor];
            m_MouseDownMonitor = nil;
        }
        if (m_WebView) {
            [m_WebView setNavigationDelegate:nil];
            [m_WebView removeFromSuperview];
            m_WebView = nil;
        }
        if (m_NavigationDelegate) {
            m_NavigationDelegate = nil;
        }
        if (m_DragSource) {
            m_DragSource = nil;
        }
        m_Initialized = false;
    }

private:
    Window* m_ParentWindow = nullptr;
    WKWebView* m_WebView = nil;
    NSView* m_HostView = nil;
    YouTubeNavigationDelegate* m_NavigationDelegate = nil;
    WebViewFileDragSource* m_DragSource = nil;
    id m_MouseDownMonitor = nil;
    std::function<void()> m_OnMouseDown;
    bool m_Initialized = false;
};

std::unique_ptr<IWebView> CreateWebView(Window* parentWindow) {
    if (!parentWindow) {
        return nullptr;
    }

    auto webView = std::make_unique<WebView_mac>(parentWindow);
    if (!webView->GetNativeView()) {
        return nullptr;
    }

    return webView;
}

} // namespace Platform
} // namespace GameEngine

#endif // __APPLE__
