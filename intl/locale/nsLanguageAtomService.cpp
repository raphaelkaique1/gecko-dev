/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsLanguageAtomService.h"

#include "mozilla/ArrayUtils.h"
#include "mozilla/Encoding.h"
#include "mozilla/intl/Locale.h"
#include "mozilla/intl/OSPreferences.h"
#include "MainThreadUtils.h"
#include "nsGkAtoms.h"
#include "nsUConvPropertySearch.h"
#include "nsUnicharUtils.h"
#include "MainThreadUtils.h"

#include <mutex>  // for call_once

using namespace mozilla;
using mozilla::intl::OSPreferences;

// List of mozilla internal x-* tags that map to themselves (see bug 256257)
static constexpr nsStaticAtom* kLangGroups[] = {
    // This list must be sorted!
    nsGkAtoms::x_armn,  nsGkAtoms::x_cyrillic, nsGkAtoms::x_devanagari,
    nsGkAtoms::x_geor,  nsGkAtoms::x_math,     nsGkAtoms::x_tamil,
    nsGkAtoms::Unicode, nsGkAtoms::x_western
    // These self-mappings are not necessary unless somebody use them to specify
    // lang in (X)HTML/XML documents, which they shouldn't. (see bug 256257)
    // x-beng=x-beng
    // x-cans=x-cans
    // x-ethi=x-ethi
    // x-guru=x-guru
    // x-gujr=x-gujr
    // x-khmr=x-khmr
    // x-mlym=x-mlym
};

// Map ISO 15924 script codes from BCP47 lang tag to mozilla's langGroups.
static constexpr struct {
  const char* mTag;
  nsStaticAtom* mAtom;
} kScriptLangGroup[] = {
    // This list must be sorted by script code!
    {"Arab", nsGkAtoms::ar},
    {"Armn", nsGkAtoms::x_armn},
    {"Beng", nsGkAtoms::x_beng},
    {"Cans", nsGkAtoms::x_cans},
    {"Cyrl", nsGkAtoms::x_cyrillic},
    {"Deva", nsGkAtoms::x_devanagari},
    {"Ethi", nsGkAtoms::x_ethi},
    {"Geok", nsGkAtoms::x_geor},
    {"Geor", nsGkAtoms::x_geor},
    {"Grek", nsGkAtoms::el},
    {"Gujr", nsGkAtoms::x_gujr},
    {"Guru", nsGkAtoms::x_guru},
    {"Hang", nsGkAtoms::ko},
    // Hani is not mapped to a specific langGroup, we prefer to look at the
    // primary language subtag in this case
    {"Hans", nsGkAtoms::Chinese},
    // Hant is special-cased in code
    // Hant=zh-HK
    // Hant=zh-TW
    {"Hebr", nsGkAtoms::he},
    {"Hira", nsGkAtoms::Japanese},
    {"Jpan", nsGkAtoms::Japanese},
    {"Kana", nsGkAtoms::Japanese},
    {"Khmr", nsGkAtoms::x_khmr},
    {"Knda", nsGkAtoms::x_knda},
    {"Kore", nsGkAtoms::ko},
    {"Latn", nsGkAtoms::x_western},
    {"Mlym", nsGkAtoms::x_mlym},
    {"Orya", nsGkAtoms::x_orya},
    {"Sinh", nsGkAtoms::x_sinh},
    {"Taml", nsGkAtoms::x_tamil},
    {"Telu", nsGkAtoms::x_telu},
    {"Thai", nsGkAtoms::th},
    {"Tibt", nsGkAtoms::x_tibt}};

StaticAutoPtr<nsLanguageAtomService> nsLanguageAtomService::sLangAtomService;

// static
nsLanguageAtomService* nsLanguageAtomService::GetService() {
  static std::once_flag sOnce;

  std::call_once(sOnce,
                 []() { sLangAtomService = new nsLanguageAtomService(); });

  return sLangAtomService.get();
}

// static
void nsLanguageAtomService::Shutdown() {
  // We only expect to be shut down by the main thread.
  MOZ_ASSERT(NS_IsMainThread());
  sLangAtomService = nullptr;
}

nsStaticAtom* nsLanguageAtomService::LookupLanguage(
    const nsACString& aLanguage) {
  nsAutoCString lowered(aLanguage);
  ToLowerCase(lowered);

  RefPtr<nsAtom> lang = NS_Atomize(lowered);
  return GetLanguageGroup(lang);
}

nsAtom* nsLanguageAtomService::GetLocaleLanguage() {
  {
    AutoReadLock lock(mLock);
    if (mLocaleLanguage) {
      return mLocaleLanguage;
    }
  }

  AutoWriteLock lock(mLock);
  if (!mLocaleLanguage) {
    AutoTArray<nsCString, 10> regionalPrefsLocales;
    // XXX Are the OSPreferences calls here safe to call from any thread?
    // In practice GetLocaleLanguage will be called early on the main thread
    // (e.g. by nsFontCache), so mLocaleLanguage should be safely initialized
    // before we try to use it from worker threads, but that may not be fully
    // guaranteed.
    if (NS_SUCCEEDED(OSPreferences::GetInstance()->GetRegionalPrefsLocales(
            regionalPrefsLocales))) {
      // use lowercase for all language atoms
      ToLowerCase(regionalPrefsLocales[0]);
      mLocaleLanguage = NS_Atomize(regionalPrefsLocales[0]);
    } else {
      nsAutoCString locale;
      OSPreferences::GetInstance()->GetSystemLocale(locale);

      ToLowerCase(locale);  // use lowercase for all language atoms
      mLocaleLanguage = NS_Atomize(locale);
    }
  }

  return mLocaleLanguage;
}

nsStaticAtom* nsLanguageAtomService::GetLanguageGroup(nsAtom* aLanguage) {
  {
    AutoReadLock lock(mLock);
    if (nsStaticAtom* atom = mLangToGroup.Get(aLanguage)) {
      return atom;
    }
  }

  AutoWriteLock lock(mLock);
  return mLangToGroup.LookupOrInsertWith(
      aLanguage, [&] { return GetUncachedLanguageGroup(aLanguage); });
}

nsStaticAtom* nsLanguageAtomService::GetUncachedLanguageGroup(
    nsAtom* aLanguage) const {
  nsAutoCString langStr;
  aLanguage->ToUTF8String(langStr);
  ToLowerCase(langStr);

  if (langStr[0] == 'x' && langStr[1] == '-') {
    // Internal x-* langGroup codes map to themselves (see bug 256257)
    for (nsStaticAtom* langGroup : kLangGroups) {
      if (langGroup == aLanguage) {
        return langGroup;
      }
      if (aLanguage->IsAsciiLowercase()) {
        continue;
      }
      // Do the slow ascii-case-insensitive comparison just if needed.
      nsDependentAtomString string(langGroup);
      if (string.EqualsASCII(langStr.get(), langStr.Length())) {
        return langGroup;
      }
    }
  } else {
    // If the lang code can be parsed as BCP47, look up its (likely) script.

    // https://bugzilla.mozilla.org/show_bug.cgi?id=1618034:
    // First strip any private subtags that would cause Locale to reject the
    // tag as non-wellformed.
    nsACString::const_iterator start, end;
    langStr.BeginReading(start);
    langStr.EndReading(end);
    if (FindInReadable("-x-"_ns, start, end)) {
      // The substring we want ends at the beginning of the "-x-" subtag.
      langStr.Truncate(start.get() - langStr.BeginReading());
    }

    intl::Locale loc;
    auto result = intl::LocaleParser::TryParse(langStr, loc);
    if (!result.isOk()) {
      // Did the author (wrongly) use '_' instead of '-' to separate subtags?
      // If so, fix it up and re-try parsing.
      if (langStr.Contains('_')) {
        langStr.ReplaceChar('_', '-');

        // Throw away the partially parsed locale and re-start parsing.
        loc = {};
        result = intl::LocaleParser::TryParse(langStr, loc);
      }
    }
    if (result.isOk() && loc.Canonicalize().isOk()) {
      // Fill in script subtag if not present.
      if (loc.Script().Missing()) {
        if (loc.AddLikelySubtags().isErr()) {
          // Fall back to x-unicode if no match was found
          return nsGkAtoms::Unicode;
        }
      }
      // Traditional Chinese has separate prefs for Hong Kong / Taiwan;
      // check the region subtag.
      if (loc.Script().EqualTo("Hant")) {
        if (loc.Region().EqualTo("HK")) {
          return nsGkAtoms::HongKongChinese;
        }
        return nsGkAtoms::Taiwanese;
      }
      // Search list of known script subtags that map to langGroup codes.
      size_t foundIndex;
      Span<const char> scriptAsSpan = loc.Script().Span();
      nsDependentCSubstring script(scriptAsSpan.data(), scriptAsSpan.size());
      if (BinarySearchIf(
              kScriptLangGroup, 0, std::size(kScriptLangGroup),
              [script](const auto& entry) -> int {
                return Compare(script, nsDependentCString(entry.mTag));
              },
              &foundIndex)) {
        return kScriptLangGroup[foundIndex].mAtom;
      }
      // Script subtag was not recognized (includes "Hani"); check the language
      // subtag for CJK possibilities so that we'll prefer the appropriate font
      // rather than falling back to the browser's hardcoded preference.
      if (loc.Language().EqualTo("zh")) {
        if (loc.Region().EqualTo("HK")) {
          return nsGkAtoms::HongKongChinese;
        }
        if (loc.Region().EqualTo("TW")) {
          return nsGkAtoms::Taiwanese;
        }
        return nsGkAtoms::Chinese;
      }
      if (loc.Language().EqualTo("ja")) {
        return nsGkAtoms::Japanese;
      }
      if (loc.Language().EqualTo("ko")) {
        return nsGkAtoms::ko;
      }
    }
  }

  // Fall back to x-unicode if no match was found
  return nsGkAtoms::Unicode;
}
