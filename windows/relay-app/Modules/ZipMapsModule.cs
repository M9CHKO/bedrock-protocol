namespace CpeRelay.Windows;
internal sealed class ZipMapsModule : RelayModule
{
    internal ZipMapsModule(MapQueuePanel panel) : base("zip-maps", "Карты из ZIP", "Крафт, показ и складывание карт из архива.") => Card(panel);
}
