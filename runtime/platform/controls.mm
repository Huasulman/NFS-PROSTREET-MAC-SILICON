// Ventana del menú Controles: qué tecla hace cada acción del mando. Ver input.h.
//
// Cada fila es una acción con dos casillas (tecla principal y alternativa). Al hacer clic en una
// casilla espera una tecla (también Shift, Control u Option) y la asigna; otro clic la deja vacía.

#import <Carbon/Carbon.h>
#import <Cocoa/Cocoa.h>

#include "controls.h"
#include "input.h"

namespace
{
    // Nombre de una tecla según la distribución de teclado actual (p. ej. "Ñ" en un teclado
    // español); las que no escriben nada, por su nombre.
    NSString* KeyName(uint16_t keyCode)
    {
        if (keyCode == platform::kNoKey)
            return @"—";
        switch (keyCode)
        {
        case kVK_Return: return @"Intro";
        case kVK_Tab: return @"Tab";
        case kVK_Space: return @"Espacio";
        case kVK_Delete: return @"Borrar";
        case kVK_ForwardDelete: return @"Supr";
        case kVK_Escape: return @"Esc";
        case kVK_Shift: return @"Shift izq.";
        case kVK_RightShift: return @"Shift der.";
        case kVK_Control: return @"Control izq.";
        case kVK_RightControl: return @"Control der.";
        case kVK_Option: return @"Option izq.";
        case kVK_RightOption: return @"Option der.";
        case kVK_LeftArrow: return @"←";
        case kVK_RightArrow: return @"→";
        case kVK_UpArrow: return @"↑";
        case kVK_DownArrow: return @"↓";
        case kVK_Home: return @"Inicio";
        case kVK_End: return @"Fin";
        case kVK_PageUp: return @"Re Pág";
        case kVK_PageDown: return @"Av Pág";
        case kVK_ANSI_KeypadEnter: return @"Intro (teclado num.)";
        case kVK_F1: return @"F1"; case kVK_F2: return @"F2"; case kVK_F3: return @"F3"; case kVK_F4: return @"F4";
        case kVK_F5: return @"F5"; case kVK_F6: return @"F6"; case kVK_F7: return @"F7"; case kVK_F8: return @"F8";
        case kVK_F9: return @"F9"; case kVK_F10: return @"F10"; case kVK_F11: return @"F11"; case kVK_F12: return @"F12";
        default: break;
        }
        // Lo que escribe la tecla sin modificadores en la distribución actual.
        TISInputSourceRef source = TISCopyCurrentKeyboardLayoutInputSource();
        NSString* name = nil;
        if (source)
        {
            CFDataRef layout = static_cast<CFDataRef>(TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData));
            if (layout)
            {
                UInt32 deadKeys = 0;
                UniChar chars[4];
                UniCharCount length = 0;
                if (UCKeyTranslate(reinterpret_cast<const UCKeyboardLayout*>(CFDataGetBytePtr(layout)), keyCode, kUCKeyActionDisplay, 0,
                        LMGetKbdType(), kUCKeyTranslateNoDeadKeysBit, &deadKeys, 4, &length, chars) == noErr && length > 0)
                    name = [[NSString stringWithCharacters:chars length:length] uppercaseString];
            }
            CFRelease(source);
        }
        const bool keypad = (keyCode >= kVK_ANSI_KeypadDecimal && keyCode <= kVK_ANSI_Keypad9);
        if (name.length && ![name isEqualToString:@" "])
            return keypad ? [name stringByAppendingString:@" (teclado num.)"] : name;
        return [NSString stringWithFormat:@"Tecla %u", keyCode];
    }

    // Teclas modificadoras que se pueden asignar, con su bit en NSEvent.modifierFlags.
    NSEventModifierFlags ModifierFlag(uint16_t keyCode)
    {
        switch (keyCode)
        {
        case kVK_Shift: case kVK_RightShift: return NSEventModifierFlagShift;
        case kVK_Control: case kVK_RightControl: return NSEventModifierFlagControl;
        case kVK_Option: case kVK_RightOption: return NSEventModifierFlagOption;
        default: return 0;
        }
    }
}

@interface ControlsWindowController : NSWindowController <NSWindowDelegate>
@property(nonatomic, strong) NSMutableArray<NSButton*>* keyButtons; // acción * kKeySlots + casilla
@property(nonatomic) NSInteger capturing;                            // casilla que espera tecla, o -1
@property(nonatomic, strong) id monitor;
@property(nonatomic, strong) NSTextField* hint;
@end

@implementation ControlsWindowController

- (instancetype)init
{
    NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 520, 600)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable backing:NSBackingStoreBuffered defer:NO];
    window.title = @"Controles del teclado";
    self = [super initWithWindow:window];
    window.delegate = self;
    self.capturing = -1;
    self.keyButtons = [NSMutableArray array];

    NSGridView* grid = [NSGridView gridViewWithNumberOfColumns:3 rows:0];
    grid.rowSpacing = 6;
    grid.columnSpacing = 12;
    NSTextField* (^label)(NSString*, BOOL) = ^(NSString* text, BOOL bold) {
        NSTextField* field = [NSTextField labelWithString:text];
        if (bold)
            field.font = [NSFont boldSystemFontOfSize:NSFont.systemFontSize];
        return field;
    };
    [grid addRowWithViews:@[ label(@"Acción", YES), label(@"Tecla", YES), label(@"Alternativa", YES) ]];
    for (uint32_t action = 0; action < platform::KeyActionCount(); action++)
    {
        NSMutableArray* row = [NSMutableArray arrayWithObject:label([NSString stringWithUTF8String:platform::GetKeyAction(action).name], NO)];
        for (uint32_t slot = 0; slot < platform::kKeySlots; slot++)
        {
            NSButton* button = [NSButton buttonWithTitle:@"" target:self action:@selector(keyClicked:)];
            button.tag = NSInteger(action * platform::kKeySlots + slot);
            [button.widthAnchor constraintEqualToConstant:150].active = YES;
            [self.keyButtons addObject:button];
            [row addObject:button];
        }
        [grid addRowWithViews:row];
    }

    self.hint = [NSTextField wrappingLabelWithString:@"Haz clic en una casilla y pulsa la tecla que quieras. Un segundo clic la deja vacía. "
                                                      @"Si la tecla ya estaba en otra acción, pasa a esta."];
    self.hint.textColor = NSColor.secondaryLabelColor;
    NSButton* reset = [NSButton buttonWithTitle:@"Restaurar predeterminados" target:self action:@selector(resetClicked:)];
    NSButton* close = [NSButton buttonWithTitle:@"Cerrar" target:self action:@selector(closeClicked:)];
    close.keyEquivalent = @"";
    NSStackView* buttons = [NSStackView stackViewWithViews:@[ reset, [NSView new], close ]];
    buttons.distribution = NSStackViewDistributionFill;

    NSStackView* content = [NSStackView stackViewWithViews:@[ grid, self.hint, buttons ]];
    content.orientation = NSUserInterfaceLayoutOrientationVertical;
    content.alignment = NSLayoutAttributeLeading;
    content.spacing = 14;
    content.edgeInsets = NSEdgeInsetsMake(18, 20, 18, 20);
    [buttons.widthAnchor constraintEqualToAnchor:grid.widthAnchor].active = YES;
    [self.hint.widthAnchor constraintEqualToAnchor:grid.widthAnchor].active = YES;
    // El texto de ayuda se reparte en líneas al ancho de la tabla (si no, la ensancharía).
    self.hint.preferredMaxLayoutWidth = grid.fittingSize.width;
    window.contentView = content;
    [window setContentSize:content.fittingSize]; // al tamaño de la tabla
    [self refresh];
    return self;
}

- (void)refresh
{
    for (NSButton* button in self.keyButtons)
    {
        const uint32_t action = uint32_t(button.tag) / platform::kKeySlots, slot = uint32_t(button.tag) % platform::kKeySlots;
        button.title = button.tag == self.capturing ? @"Pulsa una tecla…" : KeyName(platform::BoundKey(action, slot));
        button.highlighted = button.tag == self.capturing;
    }
}

- (void)assign:(uint16_t)keyCode
{
    const NSInteger tag = self.capturing;
    self.capturing = -1;
    platform::BindKey(uint32_t(tag) / platform::kKeySlots, uint32_t(tag) % platform::kKeySlots, keyCode);
    [self refresh];
}

- (void)keyClicked:(NSButton*)sender
{
    if (self.capturing == sender.tag)
    {
        [self assign:platform::kNoKey]; // segundo clic: vacía
        return;
    }
    self.capturing = sender.tag;
    [self refresh];
}

- (void)resetClicked:(id)sender
{
    self.capturing = -1;
    platform::ResetKeyBindings();
    [self refresh];
}

- (void)closeClicked:(id)sender
{
    [self.window performClose:nil];
}

- (void)showWindow:(id)sender
{
    platform::ReleaseAllKeys(); // el juego deja de recibir teclas mientras esta ventana tiene el foco
    if (!self.monitor)
    {
        // Mientras espera una tecla, se la queda (no llega a los botones ni hace "beep").
        __weak ControlsWindowController* weakSelf = self;
        self.monitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown | NSEventMaskFlagsChanged handler:^NSEvent*(NSEvent* event) {
            ControlsWindowController* strongSelf = weakSelf;
            if (!strongSelf || strongSelf.capturing < 0 || event.window != strongSelf.window)
                return event;
            if (event.type == NSEventTypeKeyDown)
            {
                if (event.modifierFlags & NSEventModifierFlagCommand)
                    return event; // ⌘ queda para los atajos
                [strongSelf assign:event.keyCode];
                return nil;
            }
            // Shift, Control u Option: al pulsarla (no al soltarla).
            const NSEventModifierFlags flag = ModifierFlag(event.keyCode);
            if (flag && (event.modifierFlags & flag))
                [strongSelf assign:event.keyCode];
            return nil;
        }];
    }
    [self.window center];
    [super showWindow:sender];
}

// Las teclas pueden haber cambiado desde el menú (Restaurar controles predeterminados).
- (void)windowDidBecomeKey:(NSNotification*)notification
{
    [self refresh];
}

- (void)windowWillClose:(NSNotification*)notification
{
    self.capturing = -1;
    [self refresh];
}

@end

namespace platform
{
    void ShowControlsWindow()
    {
        static ControlsWindowController* controller = nil;
        if (!controller)
            controller = [[ControlsWindowController alloc] init];
        [controller showWindow:nil];
    }

    bool IsAssignableModifier(uint16_t keyCode, uint64_t modifierFlags, bool& down)
    {
        const NSEventModifierFlags flag = ModifierFlag(keyCode);
        down = flag && (modifierFlags & flag);
        return flag != 0;
    }
}
