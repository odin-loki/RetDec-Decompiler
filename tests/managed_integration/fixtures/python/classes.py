# Tier 2: Classes and inheritance. Tests __init__, __repr__, __str__,
# property decorators, classmethods, and MRO.
class Animal:
    _registry = {}

    def __init__(self, name: str, sound: str):
        self.name = name
        self.sound = sound

    def __repr__(self) -> str:
        return f"{type(self).__name__}({self.name!r})"

    def speak(self) -> str:
        return f"{self.name} says {self.sound}"

    @classmethod
    def register(cls, subclass):
        cls._registry[subclass.__name__] = subclass
        return subclass

@Animal.register
class Dog(Animal):
    def __init__(self, name: str):
        super().__init__(name, "Woof")

    @property
    def is_good_boy(self) -> bool:
        return True

@Animal.register
class Cat(Animal):
    def __init__(self, name: str):
        super().__init__(name, "Meow")

if __name__ == "__main__":
    animals = [Dog("Rex"), Cat("Whiskers"), Dog("Buddy")]
    for a in animals:
        print(a.speak())
    print(repr(animals[0]))
    print("good_boy:", animals[0].is_good_boy)
    print("registry:", list(Animal._registry.keys()))
