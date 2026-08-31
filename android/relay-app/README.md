# CPE Relay for Android

Автономное Android-приложение для запуска Bedrock Live Relay без Termux.
Приложение содержит C++-библиотеку, слушает Minecraft Bedrock 1.21.100
(protocol 827) на фиксированном UDP-порту `19132` и подключает его к серверу,
который пользователь вводит в меню.

## Готовый APK

- [CPE Relay 1.0.0 (arm64-v8a, debug-signed)](apk/CPE-Relay-1.21.100-v1.0.0-arm64-v8a-debug.apk)
- [SHA-256](apk/CPE-Relay-1.21.100-v1.0.0-arm64-v8a-debug.apk.sha256)

Минимальная версия — Android 8.0 (API 26). APK содержит только
`arm64-v8a`, то есть предназначен для обычных современных ARM64-телефонов.
Это устанавливаемая тестовая сборка, подписанная Android debug certificate.

## Использование

1. Установить APK и открыть `CPE Relay`.
2. Ввести адрес и UDP-порт сервера назначения. По умолчанию это
   `cpe.ign.gg:19132`.
3. Нажать **Запустить relay и Minecraft**.
4. Minecraft подключается к локальному `127.0.0.1:19132`; relay также
   отвечает на LAN RakNet ping как `CPE Relay Android`.
5. При первом входе код Xbox появится в приложении и уведомлении. Кнопка
   копирует код и открывает страницу Microsoft для авторизации.

Relay работает foreground-service, поэтому продолжает работу, когда поверх
него открыт Minecraft. Остановить его можно в приложении или через действие
в уведомлении.

## Безопасность и хранение

- Xbox/Bedrock HTTP-запросы выполняются Android `HttpURLConnection` только
  по HTTPS; внешние процессы, `curl` и Termux не используются.
- Токены хранятся во внутреннем каталоге приложения, недоступном другим
  приложениям без root.
- Android backup отключён, чтобы кэш авторизации не попадал в резервную
  копию.
- Тела auth-запросов, `content_key`, токены и CDN-секреты не логируются.

## Сборка

Требуются JDK 17+, Android SDK 35, Build Tools 35, NDK
`27.2.12479018` и CMake `3.22.1`.

```text
cd android/relay-app
./gradlew :app:assembleDebug
```

На Windows используется `gradlew.bat`. Итоговый файл создаётся по адресу
`app/build/outputs/apk/debug/app-debug.apk`.

Установка через ADB:

```text
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

Gradle/Prefab поставляет OpenSSL для Android, а NDK — zlib и системные UDP
API. JNI-слой встраивает `bedrock_protocol` непосредственно в APK и передаёт
Android HTTPS-транспорт в native Xbox token managers через публичную
`XboxTokenHttpClientFactory`.
