# RTC (CMOS) и реальное время — дизайн (TODO 1.5 P0, 1.4 P0)

**Дата:** 2026-08-04
**TODO:** 1.5 «RTC (CMOS) для реального времени» (P0), 1.4 «time/date syscalls + sys_uptime» (P0), 2.4 «clock: показывать реальное время из RTC + дату» (P0)

## Goal

Дать ядру и программам доступ к стенному времени из CMOS RTC. `clock`
перестаёт показывать только аптайм; появляется команда `date`. Это фундамент
для mtime в `ls -l` (TODO 1.3/2.4, блок I).

## Scope

- `drivers/rtc.c` — чтение CMOS RTC (порты 0x70/0x71), BCD/binary-декод.
- `SYS_RTC` (34), `SYS_UPTIME` (35) в `kernel/syscall.c`.
- `programs/libaos.c/.h` — `get_rtc()`, `get_uptime()`.
- `programs/clock.c` — стенное время HH:MM:SS + дата DD.MM.YYYY.
- `programs/date.c` — новая консольная команда.
- `scripts/rtctest.py` — регрессионный тест.

Вне объёма: будильник RTC, автономная батарея/сохранение, HPET/APIC
(TODO 1.5 P2).

## Architecture

### 1. `drivers/rtc.c`

Чтение регистров CMOS:

```c
struct aos_time { int year, month, day, hour, minute, second; };

static unsigned char rtc_read(unsigned char reg) {
    outb(0x70, (unsigned char)(reg | 0x80));   // NMI masked
    return inb(0x71);
}

static unsigned int bcd2bin(unsigned char v) { return (v >> 4) * 10 + (v & 0xF); }

int rtc_get(struct aos_time *t) {
    unsigned char stb = rtc_read(0x0B);
    int binary = (stb & 0x04) != 0;            // status B bit 2
    // wait for UIP (status A bit 7) to clear
    unsigned int guard = 100000;
    while (rtc_read(0x0A) & 0x80) { if (--guard == 0) return -1; }
    unsigned char sec = rtc_read(0x00);
    unsigned char min = rtc_read(0x02);
    unsigned char hr  = rtc_read(0x04);
    unsigned char day = rtc_read(0x07);
    unsigned char mon = rtc_read(0x08);
    unsigned char yr  = rtc_read(0x09);
    unsigned char cen = rtc_read(0x32);
    if (!binary) {
        sec = (unsigned char)bcd2bin(sec);
        min = (unsigned char)bcd2bin(min);
        hr  = (unsigned char)bcd2bin(hr);
        day = (unsigned char)bcd2bin(day);
        mon = (unsigned char)bcd2bin(mon);
        yr  = (unsigned char)bcd2bin(yr);
    }
    int year = (cen >= 20 && cen <= 99) ? (int)cen * 100 + yr : 2000 + yr;
    t->year = year; t->month = mon; t->day = day;
    t->hour = hr; t->minute = min; t->second = sec;
    return 0;
}
```

- RTC в QEMU: секунды/минуты/часы/день/месяц/год + век (0x32). По умолчанию
  QEMU ставит стенное время хоста и `cen` = 20/21.
- Single-shot чтение всех полей подряд (без повторного UIP-ожидания между
  полями) — обычный компромисс; переход через полночь не критичен.
- `rtc_get` возвращает -1 при недоступности (защита от вечного цикла UIP).

### 2. Syscalls (`kernel/syscall.h`, `kernel/syscall.c`)

```c
#define SYS_RTC    34
#define SYS_UPTIME 35
```

`struct aos_time` — общий тип. Определяется в `kernel/syscall.h` и в
`programs/aosipc.h` (или libaos.h) — по существующему паттерну (дубль
структур в kernel/programs, как `aos_msg`).

Handler:

```c
case SYS_RTC: {
    struct aos_time *t = (struct aos_time *)r->ebx;
    if (in_user(t, sizeof(struct aos_time))) {
        if (rtc_get(t) == 0) r->eax = 0;
        else r->eax = -1;
    } else {
        r->eax = -5;
    }
    break;
}
case SYS_UPTIME:
    r->eax = tick / 1000;             // seconds since boot
    break;
```

`in_user` уже есть в `kernel/syscall.c`. `tick` — глобальный `volatile unsigned int`.

### 3. libaos

```c
int get_rtc(struct aos_time *t);
unsigned int get_uptime(void);
```

Обёртки `syscall(SYS_RTC, ...)` / `syscall(SYS_UPTIME, 0,...)`.

### 4. `programs/clock.c`

- Ширина окна 260×100 оставлена.
- Каждую секунду: `get_rtc(&t)`; формат `HH:MM:SS` + `DD.MM.YYYY` строками
  через `render_text` (существующий API, без printf).
- Подпись «UPTIME nn:ss» через `get_uptime()`.
- Остальная логика (MSG_CREATE/MSG_WININFO/MSG_UPDATE/MSG_CLOSE) без изменений.

### 5. `programs/date.c`

```c
void main(void) {
    struct aos_time t;
    if (get_rtc(&t) != 0) { print("rtc unavailable\n"); return; }
    print_dec(t.year); print("-"); print_dec2(t.month); ...
}
```

`print_dec2` — локальный хелпер (двузначное с ведущим нулём) через
`print_dec`/`putchar`, без printf (блок B добавит printf).
Вывод: `2026-08-04 14:30:22`.

Регистрируется как обычная программа: файл `programs/date.c`, программа в
Makefile `PROGRAMS`, бинарь `bin/date` в ramdisk.

## Error handling

- `rtc_get` == -1 (UIP-таймаут) → syscall возвращает -1; `date` печатает
  «rtc unavailable», `clock` продолжает аптайм.
- Невалидный указатель → -5 (стандарт syscall-слоя).

## Testing

- `make` собирается без warning.
- `scripts/rtctest.py`: boot ISO, терминал → `date\n`, проверка serial на
  `^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}$` и отсутствие KERNEL PANIC.
  Дополнительно запуск `clock` (dock) — не паникует; пиксель-проверка не
  нужна, только отсутствие паники.
- Существующие тесты зелёные (`make test`).

## Constraints / non-goals

- ABI syscall-номеров: 34/35 (следующие свободные после 33).
- Не трогать планировщик, PIT, будильник RTC.
- `get_tick()` остаётся (монотонный тик для sleep/yield); RTC — для
  отображения стенного времени.
