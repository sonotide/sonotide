# Примеры

Примеры лежат здесь не для галочки: каждый из них показывает отдельный слой
Sonotide и помогает быстро проверить, что фреймворк собран и работает в нужном
окружении.

## `sonotide_list_devices.exe`

Этот пример перечисляет доступные аудиоустройства и печатает устройства по
умолчанию. Он полезен, когда нужно убедиться, что `runtime` видит системный аудиостек и
корректно читает метаданные устройств.

Запуск:

```bat
build\msvc-x64-debug\examples\Debug\sonotide_list_devices.exe
```

## `sonotide_render_silence.exe`

Этот пример открывает поток вывода и отправляет в него тишину. Визуально он
может ничего не показать, поэтому его лучше воспринимать как быструю проверку
ветки вывода.

Запуск:

```bat
build\msvc-x64-debug\examples\Debug\sonotide_render_silence.exe
```

Если нужен явный результат, проверь код возврата:

```bat
build\msvc-x64-debug\examples\Debug\sonotide_render_silence.exe
echo %ERRORLEVEL%
```

## `sonotide_playback_session.exe`

Это уже пример высокого уровня. Он поднимает сессию воспроизведения, загружает
источник через Media Foundation, строит транспорт и ведёт состояние
воспроизведения.

Запуск:

```bat
build\msvc-x64-debug\examples\Debug\sonotide_playback_session.exe "C:\Windows\Media\notify.wav"
```

Для WSL или bash можно вызвать тот же бинарь через `cmd.exe /c`:

```bash
cmd.exe /c build\\msvc-x64-debug\\examples\\Debug\\sonotide_list_devices.exe
cmd.exe /c build\\msvc-x64-debug\\examples\\Debug\\sonotide_render_silence.exe
cmd.exe /c "build\\msvc-x64-debug\\examples\\Debug\\sonotide_playback_session.exe C:\\Windows\\Media\\notify.wav"
```

## Как использовать примеры

- `list_devices` удобно запускать первым, чтобы посмотреть, видит ли система
  нужные endpoints;
- `render_silence` подходит для проверки базового пути вывода через WASAPI;
- `playback_session` показывает уже тот сценарий, ради которого фреймворк и
  задумывался.
