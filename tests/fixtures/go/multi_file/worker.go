package queue

import "fmt"

type Worker interface {
	Work(Task) error
}

type LocalWorker struct{}

func (LocalWorker) Work(task Task) error {
	fmt.Println(task.IDString())
	return nil
}

type WrongSignature struct{}

func (WrongSignature) Work(Task) string {
	return ""
}

type PointerOnly struct{}

func (*PointerOnly) Work(Task) error {
	return nil
}

type GenericWorker[T any] struct{}

func (GenericWorker[T]) Work(Task) error {
	return nil
}
