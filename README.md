# Система видеоформата .avo

Собственный видеоформат с компрессией на основе разницы между кадрами.

## Особенности формата .avo

1. **Файл .avo** - архив с первым ключевым кадром
2. **Файлы .avop** - бинарные файлы с изменениями между кадрами
3. **RLE-сжатие** - для последовательных одинаковых пикселей
4. **Сетевая трансляция** - UDP-based стриминг

## Структура проекта

- `avo_codec.h/cpp` - основной кодек для кодирования/декодирования
- `network_stream.h/cpp` - сетевая трансляция
- `test_app.cpp` - тестовое приложение с интерфейсом

## Сборка на Debian 13

### 1. Установка зависимостей

```bash
sudo apt update
sudo apt install g++ libopencv-dev
sudo apt install gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly
```

### 2. Компиляция

```bash
g++ -std=c++17 -o test_app test_app.cpp avo_codec.cpp network_stream.cpp $(pkg-config --cflags --libs opencv4) -lpthread
```
