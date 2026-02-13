# **Tick-based Coroutine Scheduler (C++20)**

Высокопроизводительный асинхронный планировщик на базе C++20 корутин, вдохновленный архитектурой libuv и моделирующий поведение async/await из JavaScript.

## **Ключевые особенности**

* **Symmetric Transfer**: Использование симметричной передачи для предотвращения переполнения стека и минимизации накладных расходов планировщика.  
* **Min-Heap Timer Queue**: Очередь таймеров на базе бинарной кучи (как в Node.js) обеспечивает эффективную работу с миллионами задач.  
* **Ready/Waiters Queue Split**: Разделение очередей на готовые к выполнению и спящие задачи для масштабируемости ![][image1].  
* **Coroutine Flattening**: Автоматическое развертывание вложенных цепочек Task\<Task\<T\>\> прямо в рантайме.  
* **Exception Propagation**: Проброс исключений сквозь асинхронные границы с сохранением RAII-гарантий.  
* **Portability**: Кроссплатформенная реализация idle() с использованием инструкций pause (x86), yield (ARM) или системного fallback.

## **Архитектура**

Проект состоит из нескольких ключевых компонентов:

1. scheduler: Абстрактный интерфейс планировщика.  
2. manual\_scheduler: Реализация с поддержкой тиков и приоритетной очереди.  
3. task\<T\>: Awaitable-объект, управляющий временем жизни корутины.  
4. promise\_base: Базовое состояние для всех типов задач.

## **Как запустить**

Для компиляции требуется компилятор с поддержкой C++20 (GCC 10+, Clang 12+, MSVC 19.28+).

g++ \-std=c++20 task\_chaining.cpp \-o scheduler\_demo  
./scheduler\_demo

## **Сравнение с cppcoro**

Подробный анализ отличий нашей реализации от библиотеки cppcoro можно найти в документации проекта (см. cppcoro\_comparison.md).

[image1]: <data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAEwAAAAZCAYAAACb1MhvAAABWElEQVR4Xu2QUYrEMAxDe5Pe/5S7ZMCgEZLczDaFWfIgH7GeY7fHsdls/jvnef5w7Zv4aP/RpA57DDszvXfT7c45esq3ONkNLlzW9a2GfwaTssiVRueoWuF6ngDnuh1m6y/qo6J0eI/viPKfAue6HVLdZTkEyuNFUq/K8R3OEHb43oGe6uM7ovy3QIaE8lQN4Vy57FSN71XjzMFed0fUTm+BDAnlqRqCeXKxrjxVSyiX31AOInN+xFEeu6qGYJ7czlO1hHJ5BueMdK4u4jxXL3hJ53aeqiWUyzM4Z6RzZZFylOfqBS/pXM7qjgf9DufPvGed7oGUp2yAeXLZ43yG1F9zkjOITgpTNpjNlasc5V2l6+3yQbtDCXzYUzjPvcV11c95cgv2nOvqSOr/M3c/nN5b+iHA0hl3f0R66+5Zjidm3Dqkfgwf9lbw1JwXjw5bwLfvv9lsNo5fz2J8sHljVCAAAAAASUVORK5CYII=>
