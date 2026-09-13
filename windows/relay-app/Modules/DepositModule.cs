namespace CpeRelay.Windows;
internal sealed class DepositModule : RelayModule
{
    internal DepositModule(CheckBox enabled, CheckBox hotbar, Label status, Label speedLabel, TrackBar speed)
        : base("deposit", "Разгрузка шалкеров", "Перенос в свободные ячейки открытого сундука.")
    {
        Card(Caption("УПРАВЛЕНИЕ"), enabled, hotbar, status,
            Note("Откройте сундук. Ручное перемещение предметов приостановит разгрузку\nдо следующего открытия сундука."));
        Card(Caption("СКОРОСТЬ"), speedLabel, speed,
            Note("Вправо — быстрее. Пауза: 30–3000 мс. Для тяжёлых NBT начните с 1000 мс.\nФактическая скорость зависит от подтверждений сервера."));
    }
}
