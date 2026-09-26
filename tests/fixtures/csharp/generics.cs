using System;
using System.Collections.Generic;

namespace Storage.Generic {

public interface IEntity<TKey> {
    TKey Id { get; set; }
}

public interface IRepository<TEntity, TKey> where TEntity : class, IEntity<TKey> {
    TEntity GetById(TKey id);
    void Save(TEntity entity);
}

public class User : IEntity<int> {
    public int Id { get; set; }
    public string Name { get; set; }
}

public class Repository<TEntity, TKey> : IRepository<TEntity, TKey> where TEntity : class, IEntity<TKey> {
    private readonly List<TEntity> _items;

    public Repository() {
        _items = new List<TEntity>();
    }

    public TEntity GetById(TKey id) {
        return default;
    }

    public void Save(TEntity entity) {
        _items.Add(entity);
    }

    public TResult Transform<TResult>(TEntity entity, Func<TEntity, TResult> mapper) {
        return mapper(entity);
    }
}

public class Service {
    public void Process() {
        var repo = new Repository<User, int>();
        var user = new User();
        repo.Save(user);
    }
}

}
