namespace CpeRelay.Windows;
internal sealed class ArmorModule : RelayModule
{
    internal ArmorModule(CheckBox enabled) : base("armor", "Автоброня", "Автоматическое управление бронёй персонажа.") =>
        Card(Caption("СНАРЯЖЕНИЕ"), enabled, Note("Работает после подключения к серверу. Не зависит от No Render и плавающих панелей."));
}
