package queue

type Task struct {
	ID string
}

func (task Task) IDString() string {
	return task.ID
}
