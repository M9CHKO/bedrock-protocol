namespace CpeRelay.Windows;
internal sealed class NoRenderModule : RelayModule
{
    internal NoRenderModule(CheckBox enabled) : base("no-render", "No Render", "Управление видимостью сущностей, отдельно от карт.")
    {
        Card(Caption("СУЩНОСТИ"), enabled,
            Note("Выключите переключатель, чтобы вернуть отображение сущностей.\nИх движения и игровые взаимодействия продолжают передаваться."));
        Card(Caption("БЛОКИ И КАРТЫ"),
            Note("Данные сундуков, шалкеров-блоков и чанков не скрываются этим модулем.\nКарты всегда прогружаются постепенно — без ручного переключения."));
    }
}
