package aliasfixture

import model "example.org/project/model"

type Task = model.Task
type TaskID int

func UseTask(task Task) string {
	return model.Name(task)
}
