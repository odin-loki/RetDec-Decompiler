' Tier 2: Classes, inheritance, and interfaces. Tests VB.NET class hierarchy,
' overriding methods, and interface implementation.
Imports System.Collections.Generic

Interface IAnimal
    Function Speak() As String
    ReadOnly Property Name As String
End Interface

Class Animal
    Implements IAnimal
    Private _name As String
    Public Sub New(name As String)
        _name = name
    End Sub
    Public ReadOnly Property Name As String Implements IAnimal.Name
        Get
            Return _name
        End Get
    End Property
    Public Overridable Function Speak() As String Implements IAnimal.Speak
        Return "..."
    End Function
End Class

Class Dog
    Inherits Animal
    Public Sub New(name As String)
        MyBase.New(name)
    End Sub
    Public Overrides Function Speak() As String
        Return "Woof!"
    End Function
End Class

Class Cat
    Inherits Animal
    Public Sub New(name As String)
        MyBase.New(name)
    End Sub
    Public Overrides Function Speak() As String
        Return "Meow!"
    End Function
End Class

Module Classes
    Sub Main()
        Dim animals As New List(Of IAnimal) From {
            New Dog("Rex"),
            New Cat("Whiskers"),
            New Dog("Buddy")
        }
        For Each a As IAnimal In animals
            Console.WriteLine(a.Name & " says: " & a.Speak())
        Next
    End Sub
End Module
