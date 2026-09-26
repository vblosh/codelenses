import React, { useState } from 'react';
import { Button, Nav } from './ui-lib';

export function UserDashboard({ username, role }) {
    const [count, setCount] = useState(0);

    const handleIncrement = () => {
        setCount(count + 1);
    };

    return (
        <div className="dashboard-container">
            <header>
                <h2>User Profile: {username}</h2>
                <Nav.Bar variant="dark">
                    <Nav.Item label="Home" active={true} />
                    <Nav.Item label="Settings" active={false} />
                </Nav.Bar>
            </header>
            <main>
                <p>Current role: {role}</p>
                <Button label="Click Me" onClick={handleIncrement} count={count}>
                    <span>Increment</span>
                </Button>
            </main>
        </div>
    );
}

export default UserDashboard;
