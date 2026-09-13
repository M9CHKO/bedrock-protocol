namespace CpeRelay.Windows;
internal sealed class PlatformBuilderModule : RelayModule
{
    internal PlatformBuilderModule(PlatformPanel panel) : base("platform", "Строительство", "Платформа, маршруты и сундуки с материалами.") => Card(panel);
}
