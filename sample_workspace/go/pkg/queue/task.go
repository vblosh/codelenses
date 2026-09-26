package queue

import "time"

type TaskState int

const (
	StatePending TaskState = iota
	StateRunning
	StateDone
	StateFailed
)

type Task struct {
	ID        string
	Payload   string
	State     TaskState
	CreatedAt time.Time
}

type TaskHandler interface {
	Handle(task *Task) error
}

func NewTask(id string, payload string) *Task {
	return &Task{
		ID:        id,
		Payload:   payload,
		State:     StatePending,
		CreatedAt: time.Now(),
	}
}

func (t *Task) MarkComplete() {
	t.State = StateDone
}
