#include "keyboard-layout-manager.h"

#include <X11/XKBlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XKBrules.h>
#include <cwctype>
#include <cctype>

void KeyboardLayoutManager::PlatformSetup(const Napi::CallbackInfo& info) {
  auto env = info.Env();

  xDisplay = XOpenDisplay("");
  CHECK_VOID(
    xDisplay,
    "Could not connect to X display",
    env
  );

  xInputMethod = XOpenIM(xDisplay, 0, 0, 0);
  if (!xInputMethod) return;

  XIMStyles* styles = 0;
  if (XGetIMValues(xInputMethod, XNQueryInputStyle, &styles, NULL) || !styles) {
    return;
  }

  XIMStyle bestMatchStyle = 0;
  for (int i = 0; i < styles->count_styles; i++) {
    XIMStyle thisStyle = styles->supported_styles[i];
    if (thisStyle == (XIMPreeditNothing | XIMStatusNothing))
    {
      bestMatchStyle = thisStyle;
      break;
    }
  }
  XFree(styles);
  if (!bestMatchStyle) return;

  Window window;
  int revert_to;
  XGetInputFocus(xDisplay, &window, &revert_to);
  if (window != BadRequest) {
    xInputContext = XCreateIC(
      xInputMethod, XNInputStyle, bestMatchStyle, XNClientWindow, window,
      XNFocusWindow, window, NULL
    );
  }
}

void KeyboardLayoutManager::PlatformTeardown() {
  if (xInputContext) {
    XDestroyIC(xInputContext);
  }

  if (xInputMethod) {
    XCloseIM(xInputMethod);
  }

  XCloseDisplay(xDisplay);
  delete callback;
};

void KeyboardLayoutManager::HandleKeyboardLayoutChanged() {
}

Napi::Value KeyboardLayoutManager::GetCurrentKeyboardLayout(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  Napi::HandleScope scope(env);
  Napi::Value result;

  XkbRF_VarDefsRec vdr;
  char *tmp = NULL;
  if (XkbRF_GetNamesProp(xDisplay, &tmp, &vdr) && vdr.layout) {
    XkbStateRec xkbState;
    XkbGetState(xDisplay, XkbUseCoreKbd, &xkbState);
    if (vdr.variant) {
      result = Napi::String::New(env, std::string(vdr.layout) + "," + std::string(vdr.variant) + " [" + std::to_string(xkbState.group) + "]");
    } else {
      result = Napi::String::New(env, std::string(vdr.layout) + " [" + std::to_string(xkbState.group) + "]");
    }
  } else {
    result = env.Null();
  }

  return result;
}

Napi::Value KeyboardLayoutManager::GetCurrentKeyboardLanguage(const Napi::CallbackInfo& info) {
  // No distinction between “language” and “layout” on Linux.
  return GetCurrentKeyboardLayout(info);
}

Napi::Value KeyboardLayoutManager::GetInstalledKeyboardLanguages(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  Napi::HandleScope scope(env);
  return env.Undefined();
}

struct KeycodeMapEntry {
  uint xkbKeycode;
  const char *dom3Code;
};

#define USB_KEYMAP_DECLARATION static const KeycodeMapEntry keyCodeMap[] =
#define USB_KEYMAP(usb, evdev, xkb, win, mac, code, id) {xkb, code}

#include "keycode_converter_data.inc"

Napi::Value CharacterForNativeCode(Napi::Env env, XIC xInputContext, XKeyEvent *keyEvent, uint xkbKeycode, uint state) {
  keyEvent->keycode = xkbKeycode;
  keyEvent->state = state;

  if (xInputContext) {
    wchar_t characters[2];
    int count = XwcLookupString(xInputContext, keyEvent, characters, 2, NULL, NULL);
    if (count > 0 && !std::iswcntrl(characters[0])) {
      return Napi::String::New(env, reinterpret_cast<const uint16_t *>(characters), count);
    } else {
      return env.Null();
    }
  } else {
    // Graceful fallback for systems where no window is open or no input
    // context can be found.
    char characters[2];
    int count = XLookupString(keyEvent, characters, 2, NULL, NULL);
    if (count > 0 && !std::iscntrl(characters[0])) {
      return Napi::String::New(env, reinterpret_cast<const uint16_t *>(characters), count);
    } else {
      return env.Null();
    }
  }
}

Napi::Value KeyboardLayoutManager::GetCurrentKeymap(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  Napi::Object result = Napi::Object::New(env);
  Napi::String unmodifiedKey = Napi::String::New(env, "unmodified");
  Napi::String withShiftKey = Napi::String::New(env, "withShift");

  // Clear cached keymap.
  XMappingEvent eventMap = {MappingNotify, 0, false, xDisplay, 0, MappingKeyboard, 0, 0};
  XRefreshKeyboardMapping(&eventMap);

  XkbStateRec xkbState;
  XkbGetState(xDisplay, XkbUseCoreKbd, &xkbState);
  uint keyboardBaseState = 0x0000;
  if (xkbState.group == 1) {
    keyboardBaseState = 0x2000;
  } else if (xkbState.group == 2) {
    keyboardBaseState = 0x4000;
  }

  // Set up an event to reuse across CharacterForNativeCode calls.
  XEvent event;
  memset(&event, 0, sizeof(XEvent));
  XKeyEvent* keyEvent = &event.xkey;
  keyEvent->display = xDisplay;
  keyEvent->type = KeyPress;

  size_t keyCodeMapSize = sizeof(keyCodeMap) / sizeof(keyCodeMap[0]);
  for (size_t i = 0; i < keyCodeMapSize; i++) {
    const char *dom3Code = keyCodeMap[i].dom3Code;
    uint xkbKeycode = keyCodeMap[i].xkbKeycode;

    if (dom3Code && xkbKeycode > 0x0000) {
      Napi::String dom3CodeKey = Napi::String::New(env, dom3Code);
      Napi::Value unmodified = CharacterForNativeCode(env, xInputContext, keyEvent, xkbKeycode, keyboardBaseState);
      Napi::Value withShift = CharacterForNativeCode(env, xInputContext, keyEvent, xkbKeycode, keyboardBaseState | ShiftMask);

      if (unmodified.IsString() || withShift.IsString()) {
        Napi::Object entry = Napi::Object::New(env);
        (entry).Set(unmodifiedKey, unmodified);
        (entry).Set(withShiftKey, withShift);
        (result).Set(dom3CodeKey, entry);
      }
    }
  }

  return result;
}
