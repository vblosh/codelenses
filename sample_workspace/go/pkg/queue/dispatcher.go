package queue

import (
	"context"
	"errors"
	"sync"
)

type Dispatcher struct {
	handler TaskHandler
	tasks   []*Task
	mu      sync.Mutex
}

func NewDispatcher(handler TaskHandler) *Dispatcher {
	return &Dispatcher{
		handler: handler,
		tasks:   make([]*Task, 0),
	}
}

func (d *Dispatcher) Submit(task *Task) error {
	if task == nil {
		return errors.New("cannot submit nil task")
	}

	d.mu.Lock()
	defer d.mu.Unlock()

	d.tasks = append(d.tasks, task)
	return nil
}

func (d *Dispatcher) ProcessAll(ctx context.Context) int {
	d.mu.Lock()
	pending := make([]*Task, len(d.tasks))
	copy(pending, d.tasks)
	d.tasks = d.tasks[:0]
	d.mu.Unlock()

	processed := 0
	for _, t := range pending {
		if ctx.Err() != nil {
			break
		}
		if d.handler != nil {
			if err := d.handler.Handle(t); err == nil {
				t.MarkComplete()
				processed++
			}
		}
	}

	return processed
}
