namespace CpeRelay.Windows;
internal sealed class TotemModule : RelayModule
{
    internal TotemModule(CheckBox enabled) : base("totem", "Автототем", "Автоматическое управление тотемом персонажа.") =>
        Card(Caption("СНАРЯЖЕНИЕ"), enabled, Note("Держите запас тотемов в инвентаре. Переключатель независим от остальных модулей."));
}
