# TODO: одна `PriceToTicks`

Сейчас `price → ticks` (`llround(price / tick_size)`) скопирован в нескольких местах.
Потом вынести в **один** общий хелпер и звать его везде, а не плодить копии.

Дубли на момент заметки:

- `src/population/book_diff.cc` — локальный `ToTicks`
- `src/population/match.cc` / `match.hpp` — `PriceToTicks`
- `src/population/population.cc` — `Population::ToTicks`

Не делать новый файл «на каждый чих»; когда будешь править — собрать в один util
(например `population/price_ticks` или общий `book/` tick math) и удалить локальные копии.
