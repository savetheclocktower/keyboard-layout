#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0601

#undef WINVER
#define WINVER 0x0601

#define SPACE_SCAN_CODE 0x0039

#include "keyboard-layout-manager.h"

#include <string>
#include <cwctype>
#include <windows.h>

using namespace Napi;

std::string ToUTF8(const std::wstring& string) {
  if (string.length() < 1) {
    return std::string();
  }

  // NB: In the pathological case, each character could expand up
  // to 4 bytes in UTF8.
  int cbLen = (string.length()+1) * sizeof(char) * 4;
  char* buf = new char[cbLen];
  int retLen = WideCharToMultiByte(
    CP_UTF8,
    0,
    string.c_str(),
    -1,
    buf,
    cbLen,
    NULL,
    NULL
  );
  buf[retLen] = 0;

  std::string ret;
  ret.assign(buf);
  return ret;
}

HKL GetForegroundWindowHKL() {
  DWORD dwThreadId = 0;
  HWND hWnd = GetForegroundWindow();
  if (hWnd != NULL) {
    dwThreadId = GetWindowThreadProcessId(hWnd, NULL);
  }
  return GetKeyboardLayout(dwThreadId);
}

void KeyboardLayoutManager::HandleKeyboardLayoutChanged() {
  // no-op
}

Napi::Value KeyboardLayoutManager::GetCurrentKeyboardLayout(const Napi::CallbackInfo& info) {
  return GetCurrentKeyboardLayout(info.Env());
}


Napi::Value KeyboardLayoutManager::GetCurrentKeyboardLayout(Napi::Env env) {
  Napi::HandleScope scope(env);

  ActivateKeyboardLayout(GetForegroundWindowHKL(), 0);
  char layoutName[KL_NAMELENGTH];
  if (::GetKeyboardLayoutName(layoutName)) {
    return Napi::String::New(env, layoutName);
  } else {
    return env.Null();
  }
}

Napi::Value KeyboardLayoutManager::GetCurrentKeyboardLanguage(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  Napi::HandleScope scope(env);

  HKL layout = GetForegroundWindowHKL();

  wchar_t buf[LOCALE_NAME_MAX_LENGTH];
  std::wstring wstr;
  LCIDToLocaleName(MAKELCID((UINT)layout & 0xFFFF, SORT_DEFAULT), buf, LOCALE_NAME_MAX_LENGTH, 0);
  wstr.assign(buf);

  std::string str = ToUTF8(wstr);
  return Napi::String::New(env, str.data(), str.size());
}

Napi::Value KeyboardLayoutManager::GetInstalledKeyboardLanguages(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  Napi::HandleScope scope(env);

  int layoutCount = GetKeyboardLayoutList(0, NULL);
  HKL* layouts = new HKL[layoutCount];
  GetKeyboardLayoutList(layoutCount, layouts);

  Napi::Array result = Napi::Array::New(env, layoutCount);
  wchar_t buf[LOCALE_NAME_MAX_LENGTH];

  for (int i=0; i < layoutCount; i++) {
    std::wstring wstr;
    LCIDToLocaleName(MAKELCID((UINT)layouts[i] & 0xFFFF, SORT_DEFAULT), buf, LOCALE_NAME_MAX_LENGTH, 0);
    wstr.assign(buf);

    std::string str = ToUTF8(wstr);
    result.Set(i, Napi::String::New(env, str.data(), str.size()));
  }

  delete[] layouts;
  return result;
}

void KeyboardLayoutManager::PlatformSetup(const Napi::CallbackInfo& info) {
  // no-op
}

void KeyboardLayoutManager::PlatformTeardown() {
  // no-op
};

struct KeycodeMapEntry {
  UINT scanCode;
  const char *dom3Code;
};

#define USB_KEYMAP_DECLARATION static const KeycodeMapEntry keyCodeMap[] =
#define USB_KEYMAP(usb, evdev, xkb, win, mac, code, id) {win, code}

#include "keycode_converter_data.inc"

Napi::Value CharacterForNativeCode(
  Napi::Env env,
  HKL keyboardLayout,
  UINT keyCode,
  UINT scanCode,
  BYTE *keyboardState,
  bool shift,
  bool altGraph
) {
  memset(keyboardState, 0, 256);
  if (shift) {
    keyboardState[VK_SHIFT] = 0x80;
  }

  if (altGraph) {
    keyboardState[VK_MENU] = 0x80;
    keyboardState[VK_CONTROL] = 0x80;
  }

  wchar_t characters[5];
  int count = ToUnicodeEx(keyCode, scanCode, keyboardState, characters, 5, 0, keyboardLayout);

  // The check to detect and skip running this function for dead keys does not
  // account for modifier state. For layouts that map dead keys to AltGraph or
  // Shift-AltGraph we still have to detect and clear the key out of the
  // kernel-mode keyboard buffer so the keymap for subsequent keys is correctly
  // translated and not affected by the dead key.
  if (count == -1) { // Dead key
    // Dead keys are not cleared if both AltGraph and Shift is held down so
    // we clear this keyboard state to ensure that it is cleared correctly.
    keyboardState[VK_SHIFT] = 0x0;
    keyboardState[VK_MENU] = 0x0;
    keyboardState[VK_CONTROL] = 0x0;

    // Clear dead key out of kernel-mode keyboard buffer so subsequent
    // translations are not affected.
    UINT spaceKeyCode = MapVirtualKeyEx(SPACE_SCAN_CODE, MAPVK_VSC_TO_VK, keyboardLayout);
    ToUnicodeEx(spaceKeyCode, SPACE_SCAN_CODE, keyboardState, characters, 5, 0, keyboardLayout);

    // Don't translate dead keys.
    return env.Null();
  } else if (count > 0 && !std::iswcntrl(characters[0])) {
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, characters, count, NULL, 0, NULL, NULL);
    if (utf8Len == 0) {
      return env.Null();
    }
    std::string utf8(utf8Len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, characters, count, &utf8[0], utf8Len, NULL, NULL);
    return Napi::String::New(env, utf8);
  } else {
    return env.Null();
  }
}

Napi::Value KeyboardLayoutManager::GetCurrentKeymap(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  BYTE keyboardState[256];
  HKL keyboardLayout = GetForegroundWindowHKL();

  Napi::Object result = Napi::Object::New(env);

  size_t keyCodeMapSize = sizeof(keyCodeMap) / sizeof(keyCodeMap[0]);
  for (size_t i = 0; i < keyCodeMapSize; i++) {
    const char *dom3Code = keyCodeMap[i].dom3Code;
    UINT scanCode = keyCodeMap[i].scanCode;

    if (dom3Code && scanCode > 0x0000) {
      UINT keyCode = MapVirtualKeyEx(scanCode, MAPVK_VSC_TO_VK, keyboardLayout);

      // Detect and skip dead keys. If the most significant bit of the returned
      // character value is 1, this is a dead key. Trying to translate it to a
      // character will mutate the Windows keyboard buffer and blow away
      // pending dead keys. To avoid this bug, we just refuse to map dead keys
      // to characters.
      if (
        (MapVirtualKeyEx(keyCode, MAPVK_VK_TO_CHAR, keyboardLayout) >>
        (sizeof(UINT) * 8 - 1))
      ) {
        continue;
      }

      Napi::String dom3CodeKey = Napi::String::New(env, dom3Code);
      Napi::Value unmodified = CharacterForNativeCode(env, keyboardLayout, keyCode, scanCode, keyboardState, false, false);
      Napi::Value withShift = CharacterForNativeCode(env, keyboardLayout, keyCode, scanCode, keyboardState, true, false);
      Napi::Value withAltGraph = CharacterForNativeCode(env, keyboardLayout, keyCode, scanCode, keyboardState, false, true);
      Napi::Value withAltGraphShift = CharacterForNativeCode(env, keyboardLayout, keyCode, scanCode, keyboardState, true, true);

      if (unmodified.IsString() || withShift.IsString() || withAltGraph.IsString() || withAltGraphShift.IsString()) {
        Napi::Object entry = Napi::Object::New(env);
        (entry).Set("unmodified", unmodified);
        (entry).Set("withShift", withShift);
        (entry).Set("withAltGraph", withAltGraph);
        (entry).Set("withAltGraphShift", withAltGraphShift);

        (result).Set(dom3CodeKey, entry);
      }
    }
  }

  return result;
}
