namespace CpeRelay.Windows;
internal sealed class AutoCraftModule : RelayModule
{
    internal AutoCraftModule(CheckBox enabled, Button toggle, Button reset, Label status,
        Label craftLabel, TrackBar craftSpeed, Label windowLabel, TrackBar windowSpeed)
        : base("auto-craft", "Авто 2", "NBT-крафт на верстаке и разгрузка в сундуки.")
    {
        Card(Caption("УПРАВЛЕНИЕ"), enabled, Row(toggle, reset), status,
            Note("Включите NBT-крафт в библиотеке. Раковины и сундуки — в инвентаре.\nВерстак и сундуки должны быть в пределах 4 блоков. Закройте меню игры перед стартом."));
        Card(Caption("ТЕМП РАБОТЫ"), craftLabel, craftSpeed, windowLabel, windowSpeed,
            Note("Вправо — быстрее. Крафт: 100–5000 мс. Переходы: 300–3000 мс.\nПолные сундуки пропускаются. Без ресурсов или свободного места цикл завершится."));
    }
}
