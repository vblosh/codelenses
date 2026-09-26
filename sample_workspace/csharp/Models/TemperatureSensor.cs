using System;

namespace SampleWorkspace.Sensor;

public class TemperatureSensor : ISensor
{
    private double _currentTemperature;
    private double _calibrationOffset;

    public string DeviceId { get; private set; }
    public SensorCategory Category => SensorCategory.Temperature;
    public bool IsOnline { get; set; }
    public string Unit { get; set; } = "Celsius";

    public TemperatureSensor(string deviceId, double initialTemp = 20.0)
    {
        DeviceId = deviceId;
        _currentTemperature = initialTemp;
        _calibrationOffset = 0.0;
        IsOnline = true;
    }

    public void Calibrate(double offset)
    {
        _calibrationOffset = offset;
    }

    public double ReadMetric()
    {
        return _currentTemperature + _calibrationOffset;
    }

    public bool ValidateRange(double min, double max)
    {
        double val = ReadMetric();
        return val >= min && val <= max;
    }

    public void UpdateReading(double rawValue)
    {
        _currentTemperature = rawValue;
    }
}
