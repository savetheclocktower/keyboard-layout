#import <Cocoa/Cocoa.h>
#import <dispatch/dispatch.h>

#import <Carbon/Carbon.h>
#import "keyboard-layout-manager.h"
#include <vector>
#include <string>
#include <cctype>
#include <iostream>

static void NotificationHandler(
  CFNotificationCenterRef center,
  void *manager,
  CFStringRef name,
  const void *object,
  CFDictionaryRef userInfo
) {
  std::cout << "NotificationHandler" << std::endl;
  (static_cast<KeyboardLayoutManager *>(manager))->OnNotificationReceived();
}

static void RemoveCFObserver(void *arg) {
  auto manager = static_cast<KeyboardLayoutManager*>(arg);
  CFNotificationCenterRemoveObserver(
    CFNotificationCenterGetDistributedCenter(),
    manager,
    kTISNotifySelectedKeyboardInputSourceChanged,
    NULL
  );
}

void KeyboardLayoutManager::PlatformTeardown() {
  RemoveCFObserver(this);
}

void KeyboardLayoutManager::PlatformSetup(const Napi::CallbackInfo& info) {
  std::cout << "PlatformSetup" << std::endl;

  CFNotificationCenterAddObserver(
    CFNotificationCenterGetDistributedCenter(),
    this,
    NotificationHandler,
    kTISNotifySelectedKeyboardInputSourceChanged,
    NULL,
    CFNotificationSuspensionBehaviorDeliverImmediately
  );
}

void KeyboardLayoutManager::HandleKeyboardLayoutChanged() {
  // no-op
}

Napi::Value KeyboardLayoutManager::GetInstalledKeyboardLanguages(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  Napi::HandleScope scope(env);

  @autoreleasepool {
    std::vector<std::string> ret;

    // NB: We have to do this whole rigamarole twice, once for IMEs (i.e.
    // Japanese), and once for keyboard layouts (i.e. English).
    NSDictionary* filter = @{ (__bridge NSString *) kTISPropertyInputSourceType : (__bridge NSString *) kTISTypeKeyboardLayout };
    NSArray* keyboardLayouts = (NSArray *) TISCreateInputSourceList((__bridge CFDictionaryRef) filter, NO);

    for (size_t i=0; i < keyboardLayouts.count; i++) {
      TISInputSourceRef current = (TISInputSourceRef)[keyboardLayouts objectAtIndex:i];

      NSArray* langs = (NSArray*) TISGetInputSourceProperty(current, kTISPropertyInputSourceLanguages);
      std::string str = std::string([(NSString*)[langs objectAtIndex:0] UTF8String]);
      ret.push_back(str);
    }

    filter = @{ (__bridge NSString *) kTISPropertyInputSourceType : (__bridge NSString *) kTISTypeKeyboardInputMode };
    keyboardLayouts = (NSArray *) TISCreateInputSourceList((__bridge CFDictionaryRef) filter, NO);

    for (size_t i=0; i < keyboardLayouts.count; i++) {
      TISInputSourceRef current = (TISInputSourceRef)[keyboardLayouts objectAtIndex:i];

      NSArray* langs = (NSArray*) TISGetInputSourceProperty(current, kTISPropertyInputSourceLanguages);
      std::string str = std::string([(NSString*)[langs objectAtIndex:0] UTF8String]);
      ret.push_back(str);
    }

    auto result = Napi::Array::New(env);

    for (size_t i = 0; i < ret.size(); i++) {
      const std::string& lang = ret[i];
      result.Set(i, Napi::String::New(env, lang.data(), lang.size()));
    }

    return result;
  }
}

Napi::Value KeyboardLayoutManager::GetCurrentKeyboardLanguage(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  TISInputSourceRef source = TISCopyCurrentKeyboardInputSource();

  NSArray* langs = (NSArray*) TISGetInputSourceProperty(source, kTISPropertyInputSourceLanguages);
  NSString* lang = (NSString*) [langs objectAtIndex:0];

  return Napi::String::New(env, [lang UTF8String]);
}

Napi::Value KeyboardLayoutManager::GetCurrentKeyboardLayout(const Napi::CallbackInfo& info) {
  return GetCurrentKeyboardLayout(info.Env());
}

Napi::Value KeyboardLayoutManager::GetCurrentKeyboardLayout(Napi::Env env) {
  TISInputSourceRef source = TISCopyCurrentKeyboardInputSource();
  CFStringRef sourceId = (CFStringRef) TISGetInputSourceProperty(source, kTISPropertyInputSourceID);
  return Napi::String::New(env, [(NSString *)sourceId UTF8String]);
}

struct KeycodeMapEntry {
  UInt16 virtualKeyCode;
  const char *dom3Code;
};

#define USB_KEYMAP_DECLARATION static const KeycodeMapEntry keyCodeMap[] =
#define USB_KEYMAP(usb, evdev, xkb, win, mac, code, id) {mac, code}

#include "keycode_converter_data.inc"

Napi::Value CharacterForNativeCode(
  Napi::Env env,
  const UCKeyboardLayout* keyboardLayout,
  UInt16 virtualKeyCode,
  EventModifiers modifiers
) {
  // See https://developer.apple.com/reference/coreservices/1390584-uckeytranslate?language=objc
  UInt32 modifierKeyState = (modifiers >> 8) & 0xFF;
  UInt32 deadKeyState = 0;
  UniChar characters[4] = {0, 0, 0, 0};
  UniCharCount charCount = 0;
  OSStatus status = UCKeyTranslate(
      keyboardLayout,
      static_cast<UInt16>(virtualKeyCode),
      kUCKeyActionDown,
      modifierKeyState,
      LMGetKbdLast(),
      kUCKeyTranslateNoDeadKeysBit,
      &deadKeyState,
      sizeof(characters) / sizeof(characters[0]),
      &charCount,
      characters);

  // If the previous key was dead, translate again with the same dead key
  // state to get a printable character.
  if (status == noErr && deadKeyState != 0) {
    status = UCKeyTranslate(
        keyboardLayout,
        static_cast<UInt16>(virtualKeyCode),
        kUCKeyActionDown,
        modifierKeyState,
        LMGetKbdLast(),
        kUCKeyTranslateNoDeadKeysBit,
        &deadKeyState,
        sizeof(characters) / sizeof(characters[0]),
        &charCount,
        characters);
  }

  if (status == noErr && !std::iscntrl(characters[0])) {
    return Napi::String::New(env, reinterpret_cast<const char16_t*>(characters), static_cast<size_t>(charCount));
  } else {
    return env.Null();
  }
}

Napi::Value KeyboardLayoutManager::GetCurrentKeymap(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  std::cout << "GetCurrentKeymap" << std::endl;
  TISInputSourceRef source = TISCopyCurrentKeyboardLayoutInputSource();
  CFDataRef layoutData = static_cast<CFDataRef>(TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData));

  if (layoutData == NULL) return env.Null();

  const UCKeyboardLayout* keyboardLayout = reinterpret_cast<const UCKeyboardLayout*>(CFDataGetBytePtr(layoutData));
  auto result = Napi::Object::New(env);
  size_t keyCodeMapSize = sizeof(keyCodeMap) / sizeof(keyCodeMap[0]);

  for (size_t i = 0; i < keyCodeMapSize; i++) {
    const char *dom3Code = keyCodeMap[i].dom3Code;
    int virtualKeyCode = keyCodeMap[i].virtualKeyCode;
    if (dom3Code && virtualKeyCode < 0xffff) {
      auto dom3CodeKey = Napi::String::New(env, dom3Code);

      Napi::Value unmodified = CharacterForNativeCode(env, keyboardLayout, virtualKeyCode, 0);
      Napi::Value withShift = CharacterForNativeCode(env, keyboardLayout, virtualKeyCode, (1 << shiftKeyBit));
      Napi::Value withAltGraph = CharacterForNativeCode(env, keyboardLayout, virtualKeyCode, (1 << optionKeyBit));
      Napi::Value withAltGraphShift = CharacterForNativeCode(env, keyboardLayout, virtualKeyCode, (1 << shiftKeyBit) | (1 << optionKeyBit));

      if (unmodified.IsString() || withShift.IsString() || withAltGraph.IsString() || withAltGraphShift.IsString()) {
        auto entry = Napi::Object::New(env);
        entry.Set("unmodified", unmodified);
        entry.Set("withShift", withShift);
        entry.Set("withAltGraph", withAltGraph);
        entry.Set("withAltGraphShift", withAltGraphShift);
        result.Set(dom3CodeKey, entry);

        // Nan::Set(result, dom3CodeKey, entry);
      }
    }
  }
  return result;
}
