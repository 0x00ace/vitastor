[Документация](../../README-ru.md#документация) → [Конфигурация](../config.ru.md) → Конфигурация пулов

-----

[Read in English](pool.en.md)

# Конфигурация пулов

Настройки пулов задаются в ключе etcd `/vitastor/config/pools` в JSON-формате:

```
{
  "<Численный ID>": {
    "name": "<имя>",
    ...остальные параметры...
  }
}
```

На настройку пулов также влияют:

- [Дерево размещения OSD](#дерево-размещения)
- [Настройки отдельных OSD](#настройки-osd)

Параметры:

- [name](#name)
- [scheme](#scheme)
- [pg_size](#pg_size)
- [parity_chunks](#parity_chunks)
- [pg_minsize](#pg_minsize)
- [pg_count](#pg_count)
- [failure_domain](#failure_domain)
- [max_osd_combinations](#max_osd_combinations)
- [pg_stripe_size](#pg_stripe_size)
- [root_node](#root_node)
- [osd_tags](#osd_tags)
- [primary_affinity_tags](#primary_affinity_tags)

Примеры:

- [Реплицированный пул](#реплицированный-пул)
- [Пул с кодами коррекции ошибок 2+1](#пул-с-кодами-коррекции-ошибок)

# Дерево размещения

Дерево размещения OSD задаётся в отдельном ключе etcd `/vitastor/config/node_placement`
в следующем JSON-формате:

`
{
  "<имя узла или номер OSD>": {
    "level": "<уровень>",
    "parent": "<имя родительского узла, если есть>"
  },
  ...
}
`

Здесь, если название узла - число, считается, что это OSD. Уровень OSD
всегда равен "osd" и не может быть переопределён. Для OSD вы можете только
переопределить родительский узел. По умолчанию родителем OSD считается его хост.

Нечисловые имена узлов относятся к другим узлам дерева OSD, таким, как хосты (серверы),
стойки, датацентры и так далее.

Хосты всех OSD автоматически создаются в дереве с уровнем "host" и именем, равным имени хоста,
сообщаемым соответствующим OSD. Вы можете ссылаться на эти хосты, не заводя их
в дереве вручную.

Уровень может быть "host", "osd" или относиться к другому уровню размещения из
[placement_levels](monitor.ru.md#placement_levels).

Родительский узел нужен только для промежуточных узлов дерева.

# Настройки OSD

Настройки отдельных OSD задаются в ключах etcd `/vitastor/config/osd/<number>`
в JSON-формате `{"<key>":<value>}`.

На данный момент поддерживается одна настройка:

## reweight

- Тип: число, от 0 до 1
- По умолчанию: 1

Каждый OSD получает число PG, пропорциональное его размеру. Reweight - это
множитель для размера, используемый в процессе распределения PG.

Это значит, что OSD, сконфигурированный с reweight меньше 1 будет получать
меньше PG, чем обычно. OSD с reweight, равным 0, не будет участвовать в
хранении данных вообще. Вы можете установить reweight в 0, чтобы убрать
все данные с OSD.

# Параметры

## name

- Тип: строка
- Обязательный

Название пула.

## scheme

- Тип: строка
- Обязательный
- Возможные значения: "replicated", "xor" или "jerasure"

Схема избыточности, используемая в данном пуле.

## pg_size

- Тип: целое число
- Обязательный

Размер PG данного пула, т.е. число реплик для реплицированных пулов или
число дисков данных плюс дисков чётности для пулов EC/XOR.

## parity_chunks

- Тип: целое число

Число дисков чётности для EC/XOR пулов. Иными словами, число дисков, при
одновременной потере которых данные будут потеряны.

Игнорируется для реплицированных пулов, обязательно для EC/XOR.

## pg_minsize

- Тип: целое число
- Обязательный

Число доступных дисков для PG данного пула, при котором PG остаются активны.
Если становится невозможно размещать новые данные в PG как минимум на pg_minsize
OSD, PG деактивируется на чтение и запись. Иными словами, всегда известно,
что новые блоки данных всегда записываются как минимум на pg_minsize дисков.

FIXME: Поведение pg_minsize может быть изменено в будущем с полной деактивации
PG на перевод их в режим только для чтения.

## pg_count

- Тип: целое число
- Обязательный

Число PG для данного пула. Число должно быть достаточно большим, чтобы монитор
мог равномерно распределить по ним данные.

Обычно это означает примерно 64-128 PG на 1 OSD, т.е. pg_count можно устанавливать
равным (общему числу OSD * 100 / pg_size). Значение можно округлить до ближайшей
степени 2, чтобы потом было легче уменьшать или увеличивать число PG, умножая
или деля его на 2.

PG в Vitastor эферемерны, то есть вы можете менять их число в любой момент,
просто перезаписывая конфигурацию пулов в etcd. Однако объём перемещения данных
при этом будет минимален, если новое число PG кратно старому (или наоборот).

## failure_domain

- Тип: строка
- По умолчанию: host

Домен отказа для пула. Может быть равен "host" или "osd" или любому другому
уровню дерева OSD, задаваемому в настройке [placement_levels](monitor.ru.md#placement_levels).

Смысл домена отказа в том, что 2 копии, или 2 части одного блока данных в случае
кодов коррекции ошибок, никогда не помещаются на OSD, принадлежащие одному домену отказа.
Иными словами, домен отказа - это то, от отказа чего вы защищаете себя избыточным
хранением.

## max_osd_combinations

- Тип: целое число
- По умолчанию: 10000

Алгоритм распределения данных Vitastor основан на решателе задачи линейного
программирования. При этом для снижения сложности задачи возможные комбинации OSD
генерируются случайно и ограничиваются количеством, равным значению этого параметра.

Обычно данный параметр не требует изменений.

## pg_stripe_size

- Тип: целое число
- По умолчанию: 0

Данный параметр задаёт размер полосы "нарезки" образов на PG. Размер полосы не может
быть меньше, чем [block_size](layout-cluster.ru.md#block_size), умноженный на
(pg_size - parity_chunks) для EC-пулов или 1 для реплицированных пулов. То же
значение используется по умолчанию.

Это означает, что по умолчанию первые `pg_stripe_size = (block_size * (pg_size-parity_chunks))` байт
образа помещаются в одну PG, следующие `pg_stripe_size` байт помещаются в другую
и т.п.

Данный параметр обычно тоже не требует изменений.

## root_node

- Тип: строка

Корневой узел дерева OSD для ограничения OSD, выбираемых для пула. Задаваемый
узел должен быть предварительно задан в /vitastor/config/node_placement.

## osd_tags

- Тип: строка или массив строк

Теги OSD для ограничения OSD, выбираемых для пула. Если задаётся несколько тегов
массивом, то выбираются только OSD, у которых есть все эти теги.

## primary_affinity_tags

- Тип: строка или массив строк

Теги OSD, по которым должны выбираться OSD, предпочитаемые в качестве первичных
для PG этого пула. Имейте в виду, что для EC-пулов Vitastor также всегда
предпочитает помещать первичный OSD на один из OSD с данными, а не с чётностью.

# Примеры

## Реплицированный пул

```
{
  "1": {
    "name":"testpool",
    "scheme":"replicated",
    "pg_size":2,
    "pg_minsize":1,
    "pg_count":256,
    "failure_domain":"host"
  }
}
```

## Пул с кодами коррекции ошибок

```
{
  "2": {
    "name":"ecpool",
    "scheme":"jerasure",
    "pg_size":3,
    "parity_chunks":1,
    "pg_minsize":2,
    "pg_count":256,
    "failure_domain":"host"
  }
}
```