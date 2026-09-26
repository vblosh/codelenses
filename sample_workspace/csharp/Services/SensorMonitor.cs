using System;
using System.Collections.Generic;
using SampleWorkspace.Sensor;

namespace SampleWorkspace.Services;

public class SensorMonitor
{
    private readonly List<ISensor> _sensors = new List<ISensor>();
    public string MonitorName { get; set; }

    public SensorMonitor(string name)
    {
        MonitorName = name;
    }

    public void RegisterSensor(ISensor sensor)
    {
        if (sensor != null && !_sensors.Contains(sensor))
        {
            _sensors.Add(sensor);
        }
    }

    public double ComputeAverageReading()
    {
        if (_sensors.Count == 0)
        {
            return 0.0;
        }

        double total = 0.0;
        int activeCount = 0;

        foreach (var s in _sensors)
        {
            if (s.IsOnline)
            {
                total += s.ReadMetric();
                activeCount++;
            }
        }

        return activeCount > 0 ? total / activeCount : 0.0;
    }

    public int ActiveSensorCount => _sensors.FindAll(s => s.IsOnline).Count;
}
