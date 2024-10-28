#ifndef SRC_KEYBORD_LAYOUT_OBSERVER_H_
#define SRC_KEYBORD_LAYOUT_OBSERVER_H_

#include "napi.h"

#define CHECK(cond, msg, env)                                  \
if (!(cond)) {                                                 \
  Napi::TypeError::New(env, msg).ThrowAsJavaScriptException(); \
  return env.Null();                                           \
}

#define CHECK_VOID(cond, msg, env)                             \
if (!(cond)) {                                                 \
  Napi::TypeError::New(env, msg).ThrowAsJavaScriptException(); \
  return;                                                      \
}

#define THROW(env, msg) {                                      \
  Napi::TypeError::New(env, msg).ThrowAsJavaScriptException(); \
}

#define THROW_AND_RETURN(env, msg) {                           \
  Napi::TypeError::New(env, msg).ThrowAsJavaScriptException(); \
  return env.Null();                                           \
}


#if defined(__linux__) || defined(__FreeBSD__)
#include <X11/Xlib.h>
#endif // __linux__ || __FreeBSD__

class KeyboardLayoutManager : public Napi::ObjectWrap<KeyboardLayoutManager> {
public:
  static void Init(Napi::Env env, Napi::Object exports);
  KeyboardLayoutManager(const Napi::CallbackInfo& info);
  ~KeyboardLayoutManager();

  void OnNotificationReceived();

private:
  Napi::Value GetCurrentKeyboardLayout(const Napi::CallbackInfo& info);
  Napi::Value GetCurrentKeyboardLanguage(const Napi::CallbackInfo& info);
  Napi::Value GetInstalledKeyboardLanguages(const Napi::CallbackInfo& info);
  Napi::Value GetCurrentKeymap(const Napi::CallbackInfo& info);

  Napi::Value GetCurrentKeyboardLayout(Napi::Env env);

  void PlatformSetup(const Napi::CallbackInfo& info);
  void PlatformTeardown();

  void HandleKeyboardLayoutChanged();
  static void ProcessCallback(Napi::Env env, Napi::Function callback);
  void Cleanup();

#if defined(__linux__) || defined(__FreeBSD__)
  Display *xDisplay;
  XIC xInputContext;
  XIM xInputMethod;
#endif

  bool isFinalizing = false;
  Napi::FunctionReference callback;
  Napi::ThreadSafeFunction tsfn;
};

Napi::Object Init(Napi::Env env, Napi::Object exports);

#endif  // SRC_KEYBORD_LAYOUT_OBSERVER_H_
