# ex_blk - учебный драйвер блочного устройства

Модуль ядра Linux реализует простое блочное устройство (RAM-диск) с несколькими разделами. Он предназначен для демонстрации работы с блочными устройствами в ядре Linux с использованием API blk-mq и взаимодействия с модулем через `/proc` и `/sys`.

## 1. Структура проекта

```text
.
├── src/
│   └── ex_blk.c          # Исходный код модуля
│   └── ioctl.c           # Исходный код тестовой программы ioctl
├── checker/
│   ├── main.py           # Основной тестовый скрипт
│   └── blk_tester.py     # Тестер для блочного устройства
├── Kbuild                # Конфигурация сборки
└── Makefile              # Сборочный файл
└── task.md               # Описание задачи
```

## 2. Функционал

Основные характеристики и функции:

* использует блочное API **blk-mq** (multi-queue block layer);
* данные хранятся в оперативной памяти (RAM-диск);
* 3 раздела по 100 МБ каждый;
* формирование таблицы разделов (MBR) при загрузке;
* чтение/запись через стандартные системные вызовы;
* поддержка ioctl;
* поддержка /proc и /sys.

## 3. Сборка и загрузка

### Требования

* заголовки ядра Linux версии 6.1.130;
* компилятор и инструменты сборки (make, gcc и т.п.);
* права суперпользователя для загрузки модуля.

### Сборка

Исходники модуля лежат в [src/ex_blk.c](src/ex_blk.c). Для сборки подготовлены [Kbuild](./Kbuild) и [Makefile](./Makefile).

Сборка модуля:

```bash
make
```

Загрузка модуля:

```bash
sudo make load
```

или

```bash
sudo insmod src/ex_blk.ko
```

Также можно установить модуль в систему:

```bash
sudo make install
```

и потом

```bash
sudo modprobe ex_blk
```

### Выгрузка модуля

```bash
sudo make unload
```

или

```bash
sudo rmmod ex_blk
```

Удаление из системы:

```bash
sudo make uninstall
```

## 4. Использование устройства

После загрузки модуля в /dev появляется устройство /dev/ex_blk и три раздела: /dev/ex_blk1, /dev/ex_blk2, /dev/ex_blk3.

### Чтение/запись

Запись:

```bash
echo "Hello, World!" | sudo dd of=/dev/ex_blk1 bs=512 count=1
```

Чтение:

```bash
sudo dd if=/dev/ex_blk1 bs=512 count=1 2>/dev/null
```

### Работа с файлами

Создадим файловую систему и подключим устройство:

```bash
sudo mkfs.ext4 /dev/ex_blk1
sudo mount /dev/ex_blk1 /mnt
```

Теперь можно работать с файлами:

```bash
sudo echo "Hello, World!" > /mnt/file1.txt
sudo cat /mnt/file1.txt
```

### Проверка ioctl

Работу ioctl можно проверить, используя программу на C
[ioctl_test.c](src/ioctl_test.c).

```bash
cd src
gcc -o ioctl_test ioctl_test.c
sudo ./ioctl_test
rm ./ioctl_test
cd ..
```

Или через Python:

```bash
# BLKGETSIZE (в секторах)
sudo python3 -c "
import fcntl, struct
fd = open('/dev/ex_blk', 'rb')
size = struct.unpack('L', fcntl.ioctl(fd, 0x1260, b'\0'*8))[0]
print(f'Size in sectors: {size}')
fd.close()
"

# BLKGETSIZE64 (в байтах)
sudo python3 -c "
import fcntl, struct
fd = open('/dev/ex_blk', 'rb')
size = struct.unpack('Q', fcntl.ioctl(fd, 0x80081272, b'\0'*8))[0]
print(f'Size in bytes: {size}')
fd.close()
"
```

## 5. Интерфейсы /proc и /sys

### /proc

Модуль создает файл /proc/ex_blk/capacity с информацией о количестве секторов устройства.

```bash
$ cat /proc/ex_blk/capacity
Capacity: 614401 sectors
```

Можно записать в устройство строку и она появится в dmesg:

```bash
echo "Hello, World!" > /proc/ex_blk/capacity
```

Вывод в dmesg:

```text
[ 3941.362977] ex_blk: Written to proc file: Hello, World!
```

### Интерфейс /sys

Модуль создает файл в sysfs /sys/class/ex_blk/ex_blk/capacity

Поведение аналогично /proc

```bash
$ cat /sys/class/ex_blk/ex_blk/capacity
Capacity: 614401 sectors
```

```bash
echo "Hello, World!" > /sys/class/ex_blk/ex_blk/capacity
```

Вывод в dmesg:

```text
[ 4184.523008] ex_blk: Written to sysfs file: Hello, World!
```

## 6. Тестирование

Для проверки работоспособности модуля предусмотрен тестовый скрипт (подкаталог [checker](./checker/)):

Запуск тестов:

```bash
make check
```

Или напрямую:

```bash
python3 checker/main.py ex_blk
```

Тесты проверяют:

* Загрузку и выгрузку модуля
* Создание блочного устройства
* Доступность интерфейсов /proc и /sys
* Корректность работы ioctl команд
