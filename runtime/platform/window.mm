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

#include "input.h"
#include "window.h"
#include "../gpu/presenter.h"

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

@end

@interface ProStreetDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property(nonatomic, strong) NSWindow* window;
@property(nonatomic, strong) ProStreetView* view;
@end

@implementation ProStreetDelegate

- (void)applicationDidFinishLaunching:(NSNotification*)notification
{
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
