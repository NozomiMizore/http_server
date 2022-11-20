#ifndef THREAD_H
#define THREAD_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>

template<typename T>
class threadpool{
private:
    int thread_number;      //线程池中的线程数
    int max_requests;       //请求队列中允许的最大请求数
    pthread_t* threads;     //线程池，即线程数组，大小为thread_number
    std::list<T*> request_queue;//请求队列
    locker queue_locker;    //保护请求队列的互斥锁
    sem queue_sem;          //是否有任务需要处理
    bool stop;              //是否结束线程

    //不断从请求队列中取出任务并执行
    static void* worker(void* arg);
    void run();
public:
    threadpool(int thread_number = 8, int max_requests = 10000);
    ~threadpool();
    bool append(T* request);
};

template<typename T>
threadpool<T>::threadpool(int thread_number, int max_requests){
    if(thread_number <= 0 || max_requests <= 0)
        throw std::exception();
    this->thread_number = thread_number;
    this->max_requests = max_requests;
    this->stop = false;
    
    this->threads = new pthread_t[this->thread_number];
    if(!this->threads)
        throw std::exception(); 
    for(int i = 0; i < this->thread_number; i++){
        //循环创建线程，并将工作线程按要求进行运行
        if(pthread_create(this->threads + i, NULL, worker, this) != 0){
            delete[] this->threads;
            throw std::exception();
        }
        //让线程分离  ----工作线程自动退出,无需单独回收
        if(pthread_detach(this->threads[i])){
            delete[] this->threads;
            throw std::exception();
        }
        printf("%dth thread has been created\n", i);
    }
}

template<typename T>
threadpool<T>::~threadpool(){
    delete[] this->threads;
    this->stop = true;
}

//将“待办工作”加入到请求队列
template<typename T>
bool threadpool<T>::append(T* request){
    queue_locker.lock();
    if(request_queue.size() > max_requests){
        queue_locker.unlock();
        return false;
    }
    request_queue.push_back(request);
    queue_locker.unlock();
    queue_sem.post();
    return true;
}

//线程回调函数/工作函数，arg其实是this
template<typename T>
void* threadpool<T>::worker(void* arg){
    threadpool* pool = (threadpool*) arg; //将参数强转为线程池类，调用成员方法
    pool->run();
    return pool;
}
/*
被回调函数调用
不断等待任务队列中的新任务，然后加锁取任务->取到任务并解锁->执行任务
*/
template<typename T>
void threadpool<T>::run(){
    while(!stop){
        queue_sem.wait();           //信号量等待
        queue_locker.lock();        //被唤醒后加上互斥锁
        if(request_queue.empty()){
            queue_locker.unlock();
            continue;
        }
        T* request = request_queue.front();
        request_queue.pop_front();
        queue_locker.unlock();
        if(!request)
            continue;
        request->process();
    }
}
#endif
