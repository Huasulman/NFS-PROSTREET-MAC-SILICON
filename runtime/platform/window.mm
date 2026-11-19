// Ventana de macOS con Metal: muestra el último fotograma que presentó el juego.
// También implementa la escritura de PNG para la captura de fotogramas (ImageIO).

#import <Cocoa/Cocoa.h>
#import <GameController/GameController.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "controls.h"
#include "input.h"
#include "window.h"
#include "../gpu/presenter.h"
#include "../gpu/renderer.h"

namespace gpu
{
    void SavePng(const std::string& path, const Frame& frame)
    {
        CGColorSpaceRef colorSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef context = CGBitmapContextCreate((void*)frame.pixels.data(), frame.width, frame.height, 8,
            frame.width * 4, colorSpace, uint32_t(kCGImageAlphaPremultipliedFirst) | uint32_t(kCGBitmapByteOrder32Little));
        CGImageRef image = CGBitmapContextCreateImage(context);
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
        CGImageDestinationRef destination = CGImageDestinationCreateWithURL((__bridge CFURLRef)url,
            (__bridge CFStringRef)UTTypePNG.identifier, 1, nullptr);
        if (destination)
        {
            CGImageDestinationAddImage(destination, image, nullptr);
            CGImageDestinationFinalize(destination);
            CFRelease(destination);
        }
        CGImageRelease(image);
        CGContextRelease(context);
        CGColorSpaceRelease(colorSpace);
    }
}

@interface ProStreetView : NSView
@property(nonatomic, strong) id<MTLDevice> device;
@property(nonatomic, strong) id<MTLCommandQueue> queue;
@property(nonatomic, strong) id<MTLTexture> texture;
@property(nonatomic) uint64_t shownFrame;
@end

@implementation ProStreetView

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    self.wantsLayer = YES;
    self.device = MTLCreateSystemDefaultDevice();
    self.queue = [self.device newCommandQueue];

    CAMetalLayer* layer = [CAMetalLayer layer];
    layer.device = self.device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = NO; // se copia la textura al drawable con un blit
    self.layer = layer;
    return self;
}

// Sube el último fotograma (si hay uno nuevo) y lo copia al drawable.
- (void)tick
{
    gpu::Frame frame;
    if (!gpu::CopyLatestFrame(self.shownFrame, frame))
        return;
    self.shownFrame = frame.number;

    if (!self.texture || self.texture.width != frame.width || self.texture.height != frame.height)
    {
        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
            width:frame.width height:frame.height mipmapped:NO];
        self.texture = [self.device newTextureWithDescriptor:descriptor];
        ((CAMetalLayer*)self.layer).drawableSize = CGSizeMake(frame.width, frame.height);
    }
    [self.texture replaceRegion:MTLRegionMake2D(0, 0, frame.width, frame.height) mipmapLevel:0
        withBytes:frame.pixels.data() bytesPerRow:frame.width * 4];

    id<CAMetalDrawable> drawable = [(CAMetalLayer*)self.layer nextDrawable];
    if (!drawable)
        return;
    id<MTLCommandBuffer> commands = [self.queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    [blit copyFromTexture:self.texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
        sourceSize:MTLSizeMake(frame.width, frame.height, 1) toTexture:drawable.texture destinationSlice:0
        destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    [commands presentDrawable:drawable];
    [commands commit];
}

// Teclado: las pulsaciones van a platform::SetKey (sin pasarlas a super, que haría "beep").
- (BOOL)acceptsFirstResponder
{
    return YES;
}

- (void)keyDown:(NSEvent*)event
{
    platform::SetKey(event.keyCode, true);
}

- (void)keyUp:(NSEvent*)event
{
    platform::SetKey(event.keyCode, false);
}

// Shift, Control y Option no llegan como keyDown/keyUp: se pueden asignar a una acción (Controles).
- (void)flagsChanged:(NSEvent*)event
{
    bool down;
    if (platform::IsAssignableModifier(event.keyCode, event.modifierFlags, down))
        platform::SetKey(event.keyCode, down);
}

@end

@interface ProStreetDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property(nonatomic, strong) NSWindow* window;
@property(nonatomic, strong) ProStreetView* view;
@end

@implementation ProStreetDelegate

// Escalas de resolución del menú Gráficos (la etiqueta de cada opción es su número).
static const float kScales[] = { 1.0f, 1.5f };
static NSString* const kScaleDefault = @"ResolutionScale";
static NSString* const kMsaaDefault = @"Antialiasing";

// Barra de menús: la aplicación (ocultar, salir), Gráficos (resolución, antialiasing, pantalla
// completa) y Controles (teclas de cada acción del mando).
- (void)buildMenu
{
    NSMenu* bar = [[NSMenu alloc] init];

    NSMenuItem* appItem = [bar addItemWithTitle:@"" action:nil keyEquivalent:@""];
    NSMenu* appMenu = [[NSMenu alloc] init];
    [appMenu addItemWithTitle:@"Ocultar ProStreet" action:@selector(hide:) keyEquivalent:@"h"];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Salir de ProStreet" action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu = appMenu;

    NSMenuItem* graphicsItem = [bar addItemWithTitle:@"Gráficos" action:nil keyEquivalent:@""];
    NSMenu* graphics = [[NSMenu alloc] initWithTitle:@"Gráficos"];
    NSArray<NSString*>* titles = @[ @"Resolución 1x (1280 × 720)", @"Resolución 1,5x (1920 × 1080)" ];
    for (NSUInteger i = 0; i < titles.count; i++)
    {
        NSMenuItem* item = [graphics addItemWithTitle:titles[i] action:@selector(chooseScale:)
            keyEquivalent:[NSString stringWithFormat:@"%lu", (unsigned long)i + 1]];
        item.target = self;
        item.tag = NSInteger(i);
    }
    [graphics addItem:[NSMenuItem separatorItem]];
    NSMenuItem* msaa = [graphics addItemWithTitle:@"Antialiasing (MSAA 4x)" action:@selector(toggleAntialiasing:) keyEquivalent:@"a"];
    msaa.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
    msaa.target = self;
    [graphics addItem:[NSMenuItem separatorItem]];
    NSMenuItem* fullScreen = [graphics addItemWithTitle:@"Pantalla completa" action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
    fullScreen.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagControl;
    graphicsItem.submenu = graphics;

    NSMenuItem* controlsItem = [bar addItemWithTitle:@"Controles" action:nil keyEquivalent:@""];
    NSMenu* controls = [[NSMenu alloc] initWithTitle:@"Controles"];
    NSMenuItem* configure = [controls addItemWithTitle:@"Configurar teclado…" action:@selector(configureControls:) keyEquivalent:@"k"];
    configure.target = self;
    NSMenuItem* reset = [controls addItemWithTitle:@"Restaurar controles predeterminados" action:@selector(resetControls:) keyEquivalent:@""];
    reset.target = self;
    controlsItem.submenu = controls;

    NSApp.mainMenu = bar;
}

- (void)chooseScale:(NSMenuItem*)sender
{
    const float scale = kScales[sender.tag];
    gpu::SetResolutionScale(scale);
    [NSUserDefaults.standardUserDefaults setFloat:scale forKey:kScaleDefault];
}

- (void)configureControls:(NSMenuItem*)sender
{
    platform::ShowControlsWindow();
}

- (void)resetControls:(NSMenuItem*)sender
{
    platform::ResetKeyBindings();
}

- (void)toggleAntialiasing:(NSMenuItem*)sender
{
    const bool enabled = !gpu::Antialiasing();
    gpu::SetAntialiasing(enabled);
    [NSUserDefaults.standardUserDefaults setBool:enabled forKey:kMsaaDefault];
}

// Marca la escala y el antialiasing en uso.
- (BOOL)validateMenuItem:(NSMenuItem*)item
{
    if (item.action == @selector(chooseScale:))
        item.state = kScales[item.tag] == gpu::ResolutionScale() ? NSControlStateValueOn : NSControlStateValueOff;
    if (item.action == @selector(toggleAntialiasing:))
        item.state = gpu::Antialiasing() ? NSControlStateValueOn : NSControlStateValueOff;
    return YES;
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification
{
    // La escala y el antialiasing elegidos la última vez (las variables de entorno mandan).
    if (!std::getenv("PROSTREET_SCALE") && [NSUserDefaults.standardUserDefaults objectForKey:kScaleDefault])
        gpu::SetResolutionScale([NSUserDefaults.standardUserDefaults floatForKey:kScaleDefault]);
    if (!std::getenv("PROSTREET_NO_MSAA") && [NSUserDefaults.standardUserDefaults objectForKey:kMsaaDefault])
        gpu::SetAntialiasing([NSUserDefaults.standardUserDefaults boolForKey:kMsaaDefault]);
    [self buildMenu];

    const NSRect frame = NSMakeRect(0, 0, 1280, 720);
    self.window = [[NSWindow alloc] initWithContentRect:frame
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @"Need for Speed ProStreet (ProStreet Recomp)";
    self.window.delegate = self;
    self.window.contentAspectRatio = NSMakeSize(16, 9);
    self.view = [[ProStreetView alloc] initWithFrame:frame];
    self.window.contentView = self.view;
    [self.window center];
    [self.window makeKeyAndOrderFront:nil];
    [self.window makeFirstResponder:self.view];
    [NSApp activateIgnoringOtherApps:YES];

    // Se comprueba si hay fotograma nuevo a 120 Hz: el juego presenta a 60.
    // También se leen los mandos (siguen funcionando con la ventana en segundo plano).
    GCController.shouldMonitorBackgroundEvents = YES;
    [NSTimer scheduledTimerWithTimeInterval:1.0 / 120.0 repeats:YES block:^(NSTimer*) {
        platform::PollControllers();
        [self.view tick];
    }];
}

// Al perder el foco no llegan los keyUp: se sueltan todas las teclas.
- (void)windowDidResignKey:(NSNotification*)notification
{
    platform::ReleaseAllKeys();
}

// Al salir, los hilos del juego siguen vivos: si exit() destruyera los objetos globales del
// runtime (mutex del audio, de la imagen...), alguno podría usarlos ya destruidos. Se sale sin más.
- (void)applicationWillTerminate:(NSNotification*)notification
{
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(0);
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender
{
    return YES;
}

@end

namespace platform
{
    void RunWindow()
    {
        @autoreleasepool
        {
            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
            ProStreetDelegate* delegate = [[ProStreetDelegate alloc] init];
            NSApp.delegate = delegate;
            [NSApp run];
        }
    }
}
