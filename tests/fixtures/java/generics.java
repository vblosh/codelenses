package com.example.generics;

import java.util.ArrayList;
import java.util.List;
import java.util.function.Function;

public interface Identifiable<ID extends Comparable<ID>> {
    ID getId();
}

public interface Repository<T extends Identifiable<ID>, ID extends Comparable<ID>> {
    T findById(ID id);
    void save(T entity);
    List<T> findAll();
}

public class BaseRepository<T extends Identifiable<ID>, ID extends Comparable<ID>>
    implements Repository<T, ID> {

    private final List<T> storage;

    public BaseRepository() {
        this.storage = new ArrayList<>();
    }

    @Override
    public T findById(ID id) {
        for (T item : this.storage) {
            if (item.getId().equals(id)) {
                return item;
            }
        }
        return null;
    }

    @Override
    public void save(T entity) {
        this.storage.add(entity);
    }

    @Override
    public List<T> findAll() {
        return new ArrayList<>(this.storage);
    }

    public <R> List<R> mapEntities(Function<T, R> mapper) {
        List<R> results = new ArrayList<>();
        for (T item : this.storage) {
            results.add(mapper.apply(item));
        }
        return results;
    }

    public T find(ID id) {
        return findById(id);
    }

    public T find(ID id, String filter) {
        return findById(id);
    }
}

public class Account implements Identifiable<Long> {
    private final Long id;
    private final String name;

    public Account(Long id, String name) {
        this.id = id;
        this.name = name;
    }

    @Override
    public Long getId() {
        return this.id;
    }

    public String getName() {
        return this.name;
    }
}

public class AccountService {
    public void execute() {
        BaseRepository<Account, Long> repository = new BaseRepository<>();
        Account account = new Account(1L, "Primary");
        repository.save(account);
        Account found = repository.findById(1L);
        Account item = repository.find(1L);
        Account filtered = repository.find(1L, "active");
        List<String> names = repository.mapEntities(Account::getName);
    }
}
