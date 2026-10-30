# LiveDelay — aplikacja

Okno obok Streamlabs Desktop. Silnik opóźnienia nie jest w tym repozytorium.

`livedelay.dll` powstaje w repozytorium wtyczki (`livedelay`, `cargo build --release`). Ten program tylko ją ładuje. Zasada jest w [AGENTS.md](AGENTS.md).

## Budowanie

W repozytorium wtyczki:

```
cargo build --release
```

Tutaj:

```
build.bat
```

Wynik: `build\Release\livedelay.exe` i obok niego skopiowana `livedelay.dll`.

## Streamlabs

1. Uruchom `livedelay.exe` i zostaw je włączone (ikona przy zegarze).
2. Streamlabs → Ustawienia → Transmisja → własny serwer.
3. Wpisz adres i klucz, które pokazuje okno. Klucz to `ingest_key` z LiveDelay (zwykle `ld0`), nie klucz Twitcha.
4. Uzbrój, potem Włącz opóźnienie. Na żywo wraca od razu.

Adresy platform i ich klucze zostają w `%APPDATA%\LiveDelay\livedelay.ini`.

## Widgety

Dolny pasek okna (i menu przy zegarze) prowadzi do stron, które wystawia silnik:

- **Podgląd** — `/live-preview`: to, co przychodzi ze Streamlabs, i to, co widzą widzowie, obok siebie.
- **Overlay** — `/overlay`: licznik opóźnienia dla widzów. *Kopiuj overlay* wrzuca adres do schowka pod źródło Przeglądarka.
- **Edytor motywu** — `/edit-theme`: wygląd overlaya, zapisywany w silniku.

Nie uruchamiaj tego programu w tym samym czasie co wtyczkę w OBS. Oba słuchają na `127.0.0.1:1935` i `127.0.0.1:7799`.
