import React from "react";

export interface ButtonProps {
    title: string;
    onClick: () => void;
}

export function CustomButton(props: ButtonProps) {
    return <button onClick={props.onClick}>{props.title}</button>;
}

export const Feed = {
    Header: (props: { label: string }) => <h1>{props.label}</h1>,
};

export function App() {
    const handleClick = () => {
        console.log("clicked");
    };

    return (
        <div className="container">
            <Feed.Header label="Welcome" />
            <CustomButton title="Click Me" onClick={handleClick} />
        </div>
    );
}
