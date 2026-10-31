# LiveDelay — aplikacja (exe)

To repozytorium jest tylko oknem. Silnik opóźnienia tu nie mieszka.

## Wspólne z wtyczką OBS

Silnik jest w repozytorium `livedelay` (`skullboypl/livedelay-obs`), obok tego katalogu albo jako osobny clone:

- Rust: `livedelay/src/*.rs`, `livedelay/Cargo.toml`
- DLL: `livedelay/target/release/livedelay.dll` (`cargo build --release` w tamtym repo)
- Staticlib dla wtyczki: `livedelay/target/release/livedelay.lib` (tego exe nie linkuje)

Exe ładuje `livedelay.dll` z własnego katalogu (`LoadLibrary`) i woła `ld_start`, `ld_stop`, `ld_last_error`. Reszta (Uzbrój, Włącz, Na żywo, stan, opóźnienie) idzie HTTP na adres `api` z `%APPDATA%\LiveDelay\livedelay.ini`, domyślnie `http://127.0.0.1:7799`.

## Czego nie robić

- Nie kopiuj tu plików `.rs`, `Cargo.toml`, `Cargo.lock` ani źródeł wtyczki OBS.
- Nie wklejaj `livedelay.dll` do gita. Skrypt budowania kopiuje ją z katalogu `target/release` wtyczki, jeśli tam leży.
- Nie dokładaj Qt, OBS SDK ani własnego proxy RTMP. OpenGL w tym repo to okno i przyciski.
- Ustawienia zapisu streamu (adres platformy, klucz, ingest) zostają w `livedelay.ini`, który czyta silnik. To okno ich nie dubluje w drugim formacie.

## Dwa pobrania

- Wtyczka OBS: zip z repozytorium `livedelay-obs` (dla OBS Studio).
- Ta aplikacja: exe + `livedelay.dll` z tego samego buildu silnika (dla programu streamującego na tym PC).

## Instalacja i aktualizacja

Katalog użytkownika, bez uprawnień administratora: `%LOCALAPPDATA%\LiveDelay\App`. `Install-LiveDelay-App.ps1` kopiuje tam exe i dll i dodaje skrót w menu Start. Ustawienia i klucze zostają w `%APPDATA%\LiveDelay` i odinstalowanie ich nie rusza.

Aktualizacja nie przenosi programu. Baner w oknie pobiera paczkę, zamyka proces i podmienia pliki w katalogu, z którego exe jest uruchomiony — działa tak samo dla kopii z instalatora i dla rozpakowanego zipa. Wersja aplikacji to wersja exe. Wersja wtyczki to `ld_version` z `livedelay.dll`. Adresy pobrań są w `version.json` (`app.zip`, `engine.dll`); puste pole oznacza tylko link do strony wydania.
