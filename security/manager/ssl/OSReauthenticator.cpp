/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "OSReauthenticator.h"

#include "OSKeyStore.h"
#include "nsNetCID.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/Logging.h"
#include "nsIBaseWindow.h"
#include "nsIDocShell.h"
#include "nsISupportsUtils.h"
#include "nsIWidget.h"
#include "nsServiceManagerUtils.h"
#include "nsThreadUtils.h"
#include "ipc/IPCMessageUtils.h"

NS_IMPL_ISUPPORTS(OSReauthenticator, nsIOSReauthenticator)

extern mozilla::LazyLogModule gCredentialManagerSecretLog;

using namespace mozilla;
using dom::Promise;

#if defined(XP_WIN)
#  include <combaseapi.h>
#  include <ntsecapi.h>
#  include <wincred.h>
#  include <windows.h>
struct HandleCloser {
  typedef HANDLE pointer;
  void operator()(HANDLE h) {
    if (h != INVALID_HANDLE_VALUE) {
      CloseHandle(h);
    }
  }
};
struct BufferFreer {
  typedef LPVOID pointer;
  void operator()(LPVOID b) { CoTaskMemFree(b); }
};
typedef std::unique_ptr<HANDLE, HandleCloser> ScopedHANDLE;
typedef std::unique_ptr<LPVOID, BufferFreer> ScopedBuffer;

// Get the token info holding the sid.
std::unique_ptr<char[]> GetTokenInfo(ScopedHANDLE& token) {
  DWORD length = 0;
  // https://docs.microsoft.com/en-us/windows/desktop/api/securitybaseapi/nf-securitybaseapi-gettokeninformation
  mozilla::Unused << GetTokenInformation(token.get(), TokenUser, nullptr, 0,
                                         &length);
  if (!length || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
    MOZ_LOG(gCredentialManagerSecretLog, LogLevel::Debug,
            ("Unable to obtain current token info."));
    return nullptr;
  }
  std::unique_ptr<char[]> token_info(new char[length]);
  if (!GetTokenInformation(token.get(), TokenUser, token_info.get(), length,
                           &length)) {
    MOZ_LOG(gCredentialManagerSecretLog, LogLevel::Debug,
            ("Unable to obtain current token info (second call, possible "
             "system error."));
    return nullptr;
  }
  return token_info;
}

std::unique_ptr<char[]> GetUserTokenInfo() {
  // Get current user sid to make sure the same user got logged in.
  HANDLE token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    // Couldn't get a process token. This will fail any unlock attempts later.
    MOZ_LOG(gCredentialManagerSecretLog, LogLevel::Debug,
            ("Unable to obtain process token."));
    return nullptr;
  }
  ScopedHANDLE scopedToken(token);
  return GetTokenInfo(scopedToken);
}

// Use the Windows credential prompt to ask the user to authenticate the
// currently used account.
static nsresult ReauthenticateUserWindows(const nsACString& aMessageText,
                                          const nsACString& aCaptionText,
                                          const WindowsHandle& hwndParent,
                                          /* out */ bool& reauthenticated) {
  reauthenticated = false;

  // Is used in next iteration if the previous login failed.
  DWORD err = 0;
  uint8_t numAttempts = 3;
  std::unique_ptr<char[]> userTokenInfo = GetUserTokenInfo();

  // CredUI prompt.
  CREDUI_INFOW credui = {};
  credui.cbSize = sizeof(credui);
  credui.hwndParent = reinterpret_cast<HWND>(hwndParent);
  const nsString& messageText = NS_ConvertUTF8toUTF16(aMessageText);
  credui.pszMessageText = messageText.get();
  const nsString& captionText = NS_ConvertUTF8toUTF16(aCaptionText);
  credui.pszCaptionText = captionText.get();
  credui.hbmBanner = nullptr;  // ignored

  while (!reauthenticated && numAttempts > 0) {
    --numAttempts;

    HANDLE lsa;
    // Get authentication handle for future user authentications.
    // https://docs.microsoft.com/en-us/windows/desktop/api/ntsecapi/nf-ntsecapi-lsaconnectuntrusted
    if (LsaConnectUntrusted(&lsa) != ERROR_SUCCESS) {
      MOZ_LOG(gCredentialManagerSecretLog, LogLevel::Debug,
              ("Error aquiring lsa. Authentication attempts will fail."));
      return NS_ERROR_FAILURE;
    }
    ScopedHANDLE scopedLsa(lsa);

    if (!userTokenInfo || lsa == INVALID_HANDLE_VALUE) {
      MOZ_LOG(gCredentialManagerSecretLog, LogLevel::Debug,
              ("Error setting up login and user token."));
      return NS_ERROR_FAILURE;
    }

    ULONG authPackage = 0;
    ULONG outCredSize = 0;
    LPVOID outCredBuffer = nullptr;
    BOOL save = false;

    // Get user's Windows credentials.
    // https://docs.microsoft.com/en-us/windows/desktop/api/wincred/nf-wincred-creduipromptforwindowscredentialsw
    err = CredUIPromptForWindowsCredentialsW(
        &credui, err, &authPackage, nullptr, 0, &outCredBuffer, &outCredSize,
        &save, CREDUIWIN_ENUMERATE_CURRENT_USER);
    ScopedBuffer scopedOutCredBuffer(outCredBuffer);
    if (err == ERROR_CANCELLED) {
      MOZ_LOG(gCredentialManagerSecretLog, LogLevel::Debug,
              ("Error getting authPackage for user login, user cancel."));
      return NS_OK;
    }
    if (err != ERROR_SUCCESS) {
      MOZ_LOG(gCredentialManagerSecretLog, LogLevel::Debug,
              ("Error getting authPackage for user login."));
      return NS_ERROR_FAILURE;
    }

    // Verify the credentials.
    TOKEN_SOURCE source;
    PCHAR contextName = const_cast<PCHAR>("Mozilla");
    size_t nameLength =
        std::min(TOKEN_SOURCE_LENGTH, static_cast<int>(strlen(contextName)));
    // Note that the string must not be longer than TOKEN_SOURCE_LENGTH.
    memcpy(source.SourceName, contextName, nameLength);
    // https://docs.microsoft.com/en-us/windows/desktop/api/securitybaseapi/nf-securitybaseapi-allocatelocallyuniqueid
    if (!AllocateLocallyUniqueId(&source.SourceIdentifier)) {
      MOZ_LOG(gCredentialManagerSecretLog, LogLevel::Debug,
              ("Error allocating ID for logon process."));
      return NS_ERROR_FAILURE;
    }

    NTSTATUS substs;
    void* profileBuffer = nullptr;
    ULONG profileBufferLength = 0;
    QUOTA_LIMITS limits = {0};
    LUID luid;
    HANDLE token;
    LSA_STRING name;
    name.Buffer = contextName;
    name.Length = strlen(name.Buffer);
    name.MaximumLength = name.Length;
    // https://docs.microsoft.com/en-us/windows/desktop/api/ntsecapi/nf-ntsecapi-lsalogonuser
    NTSTATUS sts = LsaLogonUser(
        scopedLsa.get(), &name, (SECURITY_LOGON_TYPE)Interactive, authPackage,
        scopedOutCredBuffer.get(), outCredSize, nullptr, &source,
        &profileBuffer, &profileBufferLength, &luid, &token, &limits, &substs);
    ScopedHANDLE scopedToken(token);
    LsaFreeReturnBuffer(profileBuffer);
    LsaDeregisterLogonProcess(scopedLsa.get());
    if (sts == ERROR_SUCCESS) {
      MOZ_LOG(gCredentialManagerSecretLog, LogLevel::Debug,
              ("User logged in successfully."));
    } else {
      MOZ_LOG(
          gCredentialManagerSecretLog, LogLevel::Debug,
          ("Login failed with %lx (%lx).", sts, LsaNtStatusToWinError(sts)));
      continue;
    }

    // The user can select any user to log-in on the authentication prompt.
    // Make sure that the logged in user is the current user.
    std::unique_ptr<char[]> logonTokenInfo = GetTokenInfo(scopedToken);
    if (!logonTokenInfo) {
      MOZ_LOG(gCredentialManagerSecretLog, LogLevel::Debug,
              ("Error getting logon token info."));
      return NS_ERROR_FAILURE;
    }
    PSID logonSID =
        reinterpret_cast<TOKEN_USER*>(logonTokenInfo.get())->User.Sid;
    PSID userSID = reinterpret_cast<TOKEN_USER*>(userTokenInfo.get())->User.Sid;
    if (EqualSid(userSID, logonSID)) {
      MOZ_LOG(gCredentialManagerSecretLog, LogLevel::Debug,
              ("Login successfully (correct user)."));
      reauthenticated = true;
      break;
    }
  }
  return NS_OK;
}
#endif  // XP_WIN

static nsresult ReauthenticateUser(const nsACString& prompt,
                                   const nsACString& caption,
                                   const WindowsHandle& hwndParent,
                                   /* out */ bool& reauthenticated) {
  reauthenticated = false;
#if defined(XP_WIN)
  return ReauthenticateUserWindows(prompt, caption, hwndParent,
                                   reauthenticated);
#elif defined(XP_MACOSX)
  return ReauthenticateUserMacOS(prompt, reauthenticated);
#endif  // Reauthentication is not implemented for this platform.
  return NS_OK;
}

static void BackgroundReauthenticateUser(RefPtr<Promise>& aPromise,
                                         const nsACString& aMessageText,
                                         const nsACString& aCaptionText,
                                         const WindowsHandle& hwndParent) {
  nsAutoCString recovery;
  bool reauthenticated;
  nsresult rv = ReauthenticateUser(aMessageText, aCaptionText, hwndParent,
                                   reauthenticated);
  nsCOMPtr<nsIRunnable> runnable(NS_NewRunnableFunction(
      "BackgroundReauthenticateUserResolve",
      [rv, reauthenticated, aPromise = std::move(aPromise)]() {
        if (NS_FAILED(rv)) {
          aPromise->MaybeReject(rv);
        } else {
          aPromise->MaybeResolve(reauthenticated);
        }
      }));
  NS_DispatchToMainThread(runnable.forget());
}

NS_IMETHODIMP
OSReauthenticator::AsyncReauthenticateUser(const nsACString& aMessageText,
                                           const nsACString& aCaptionText,
                                           mozIDOMWindow* aParentWindow,
                                           JSContext* aCx,
                                           Promise** promiseOut) {
  NS_ENSURE_ARG_POINTER(aCx);

  RefPtr<Promise> promiseHandle;
  nsresult rv = GetPromise(aCx, promiseHandle);
  if (NS_FAILED(rv)) {
    return rv;
  }

  WindowsHandle hwndParent = 0;
  if (aParentWindow) {
    nsPIDOMWindowInner* win = nsPIDOMWindowInner::From(aParentWindow);
    nsIDocShell* docShell = win->GetDocShell();
    if (docShell) {
      nsCOMPtr<nsIBaseWindow> baseWindow = do_QueryInterface(docShell);
      if (baseWindow) {
        nsCOMPtr<nsIWidget> widget;
        baseWindow->GetMainWidget(getter_AddRefs(widget));
        if (widget) {
          hwndParent = reinterpret_cast<WindowsHandle>(
              widget->GetNativeData(NS_NATIVE_WINDOW));
        }
      }
    }
  }

  nsCOMPtr<nsIRunnable> runnable(NS_NewRunnableFunction(
      "BackgroundReauthenticateUser",
      [promiseHandle, aMessageText = nsAutoCString(aMessageText),
       aCaptionText = nsAutoCString(aCaptionText), hwndParent]() mutable {
        BackgroundReauthenticateUser(promiseHandle, aMessageText, aCaptionText,
                                     hwndParent);
      }));

  nsCOMPtr<nsIEventTarget> target(
      do_GetService(NS_STREAMTRANSPORTSERVICE_CONTRACTID));
  if (!target) {
    return NS_ERROR_FAILURE;
  }
  rv = target->Dispatch(runnable, NS_DISPATCH_NORMAL);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  promiseHandle.forget(promiseOut);
  return NS_OK;
}
