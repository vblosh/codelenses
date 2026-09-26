def handle_dynamic(dispatcher, event):
    name = event.header.name
    timestamp = event.header.timestamp
    payload = event.payload
    result = dispatcher.dispatch(event)
    fallback = dispatcher.default_handler(payload)
    return result
