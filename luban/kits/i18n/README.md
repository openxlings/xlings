# i18n

Words in a person's language: catalogues by domain and language, a fallback
chain to English (`zh-CN` -> `zh` -> `en`), a check of a domain's catalogues,
words that come with data (`pick`), and a glossary of terms every language
writes in English. The mechanism every program shares; the words are each
program's.

```cpp
import i18n;

constexpr i18n::Entry kZh[] = {{"Apps", "应用"}, {"Workspace {}", "工作区 {}"}};
i18n::set_default_domain("xdesk");
i18n::register_catalogue("xdesk", "zh", kZh);

i18n::tr("Apps");               // "应用" when the language is Chinese, else "Apps"
i18n::trf("Workspace {}", 3);   // "工作区 3"
i18n::check("xdesk");           // what is wrong with the catalogues: nothing
```

Keys are stable: a domain's dotted keys (`luban.os.quick.create`), or the
English text itself, which is then its own English. A key nobody has renders
as itself -- never a blank. The language is an override variable, then the
setting, then the system's (the environment's `LC_ALL`, `LC_MESSAGES`,
`LANG`, unless the program gives its own source: xlings reads Windows' user
locale through its platform module). It depends on the C++ standard library
only. `xlings.i18n` is it with xlings' domain the default; xdesk's words are
`xdesk.words`.
