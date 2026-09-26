class Container:
    category = "general"

    class NestedItem:
        def __init__(self, name: str):
            self.name = name

        def get_name(self) -> str:
            return self.name

    def create_item(self, name: str) -> NestedItem:
        return self.NestedItem(name)

def build_multiplier(factor: int):
    offset = 10

    def multiply(val: int) -> int:
        def inner_step() -> int:
            return val * factor + offset
        return inner_step()

    return multiply
