using System;

namespace Services.Client {

[Serializable]
public partial class ServiceClient {
    private string _endpoint;

    public ServiceClient(string endpoint) {
        _endpoint = endpoint;
        OnInitialized();
    }

    partial void OnInitialized();

    public void Connect() {
        SendPing();
    }
}

public partial class ServiceClient {
    public int TimeoutSeconds { get; set; }

    partial void OnInitialized() {
        TimeoutSeconds = 30;
    }

    private void SendPing() {
    }
}

}
