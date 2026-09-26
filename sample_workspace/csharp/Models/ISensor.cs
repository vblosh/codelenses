namespace SampleWorkspace.Sensor;

public enum SensorCategory
{
    Temperature,
    Humidity,
    Pressure
}

public interface ISensor
{
    string DeviceId { get; }
    SensorCategory Category { get; }
    bool IsOnline { get; }

    double ReadMetric();
    bool ValidateRange(double min, double max);
}
